#include "fsl_i2c.h"
#include "fsl_wm8904.h"
#include "fsl_sysctl.h"
#include "fsl_i2s.h"
#include "fsl_i2s_dma.h"
#include "fsl_powerquad.h"

#include "board.h"
#include "sysdep_pcm.h"

#include <math.h>

extern volatile int user_break;
char uart_getc(USART_Type *base);

void codec_set_master(wm8904_handle_t *handle, int master);

/*******************************************************************************
 * PCM AUDIO
 ******************************************************************************/
// sampling frequency
static unsigned int	sample_freq=PCM_SMPLFREQ_32000HZ;

// I2C4 used to send commands to CODEC
#define CODEC_I2C_INSTANCE			4
#define CODEC_I2C_CLOCK_FREQ		12000000
// I2S used to exchange sound samples with CODEC
#define I2S_CLOCK_DIVIDER 			(CLOCK_GetPll0OutFreq() / ((int)sample_freq) / 16U / 2U)

// Audio codec config
#define WM8904_SWAP_UINT16_BYTE_SEQUENCE(x) ((((x) & 0x00ffU) << 8U) | (((x) & 0xff00U) >> 8U))
#define WM8904_MAP_SAMPLERATE(x)          \
    ((x) == kWM8904_SampleRate8kHz ?      \
         8000U :                          \
         (x) == kWM8904_SampleRate12kHz ? \
         12000U :                         \
         (x) == kWM8904_SampleRate16kHz ? \
         16000U :                         \
         (x) == kWM8904_SampleRate24kHz ? \
         24000U :                         \
         (x) == kWM8904_SampleRate32kHz ? \
         32000U :                         \
         (x) == kWM8904_SampleRate48kHz ? \
         48000U :                         \
         (x) == kWM8904_SampleRate11025Hz ? 11025U : (x) == kWM8904_SampleRate22050Hz ? 22050U : 44100U)
#define WM8904_MAP_BITWIDTH(x) \
    ((x) == kWM8904_BitWidth16 ? 16 : (x) == kWM8904_BitWidth20 ? 20 : (x) == kWM8904_BitWidth24 ? 24 : 32)

wm8904_handle_t wm8904Handle;
wm8904_config_t wm8904Config = {
    .i2cConfig    = {.codecI2CInstance = CODEC_I2C_INSTANCE, .codecI2CSourceClock = CODEC_I2C_CLOCK_FREQ},
    .recordSource = kWM8904_RecordSourceLineInput,
    .recordChannelLeft  = kWM8904_RecordChannelLeft2,
    .recordChannelRight = kWM8904_RecordChannelRight2,
    .playSource         = kWM8904_PlaySourceDAC,
    .slaveAddress       = WM8904_I2C_ADDRESS,
    .protocol           = kWM8904_ProtocolI2S,
    .format             = {.sampleRate = kWM8904_SampleRate32kHz, .bitWidth = kWM8904_BitWidth16},
	.mclk_HZ			= 32768000U,
    .master             = true,
};

void codec_set_master(wm8904_handle_t *handle, int master)
{
	handle->config->master=master;
	if (master) {
	    uint32_t sysclk;

		if (handle->config->sysClkSource == kWM8904_SysClkSourceFLL) {
			sysclk = handle->config->fll->outputClock_HZ;
		} else {
			sysclk = handle->config->mclk_HZ;
		}

		WM8904_SetMasterClock(handle, sysclk, (uint32_t)(WM8904_MAP_SAMPLERATE(handle->config->format.sampleRate)),
									   (uint32_t)(WM8904_MAP_BITWIDTH(handle->config->format.bitWidth)));
	} else {
		/* BCLK/LRCLK default direction input */
		WM8904_ModifyRegister(handle, WM8904_AUDIO_IF_1, 1U << 6U, 0U);
		WM8904_ModifyRegister(handle, WM8904_AUDIO_IF_3, (uint16_t)(1UL << 11U), 0U);
	}
}

/*************************************************************************/
static dma_handle_t s_DmaRxHandle;
static dma_handle_t s_DmaTxHandle;
static i2s_dma_handle_t s_RxHandle;
static i2s_dma_handle_t s_TxHandle;


/* ping-pong rx buffers : (2 channels x SAMPLE_NB samples x sizeof(int16_t))
 * data: buffer address
 * dataSize: buffer size in bytes
 */
static volatile int pcm_rx_buf_done=0;
static int16_t pcm_rx_buf[SAMPLE_NB*6];		/* 3 buffers with 2 channels with SAMPLE_NB samples */
static i2s_transfer_t pcm_rx_xfer[3] = {
	[0] = {
		.data     = (uint8_t*)pcm_rx_buf,
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	},
	[1] = {
		.data     = (uint8_t*)(pcm_rx_buf+2*SAMPLE_NB),
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	},
	[2] = {
		.data     = (uint8_t*)(pcm_rx_buf+4*SAMPLE_NB),
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	}
};

/* ping-pong tx buffers : (2 channels x SAMPLE_NB samples x sizeof(int16_t))
 * data: buffer address
 * dataSize: buffer size in bytes
 */
static volatile int pcm_tx_buf_done=0;
static int16_t pcm_tx_buf[SAMPLE_NB*6];		/* 3 buffers with 2 channels with SAMPLE_NB samples */
static i2s_transfer_t pcm_tx_xfer[3] = {
	[0] = {
		.data     = (uint8_t*)pcm_tx_buf,
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	},
	[1] = {
		.data     = (uint8_t*)(pcm_tx_buf+2*SAMPLE_NB),
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	},
	[2] = {
		.data     = (uint8_t*)(pcm_tx_buf+4*SAMPLE_NB),
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	}
};

#define io_set(on) (GPIO_PinWrite(GPIO, 1, 11, (on) ? 1 : 0))

/* pcm_init
 *   WM8904 CODEC, I2S 6 & 7, and DMA0 initializations
 *****/
int pcm_init(void)
{
	i2s_config_t i2s_rx_config;
	i2s_config_t i2s_tx_config;
    i2c_master_config_t fc4_config = {
      .enableMaster = true,
      .baudRate_Bps = 400000,		/* mode Fast */
      .enableTimeout = false,
	  .timeout_Ms    = 35
    };

    I2C_MasterInit(I2C4, &fc4_config, CODEC_I2C_CLOCK_FREQ);
	CLOCK_EnableClock(kCLOCK_InputMux);
	CLOCK_EnableClock(kCLOCK_Sysctl);

	RESET_PeripheralReset(kFC4_RST_SHIFT_RSTn);
	RESET_PeripheralReset(kFC6_RST_SHIFT_RSTn);
	RESET_PeripheralReset(kFC7_RST_SHIFT_RSTn);
	NVIC_ClearPendingIRQ(FLEXCOMM7_IRQn);
	NVIC_ClearPendingIRQ(FLEXCOMM6_IRQn);
	EnableIRQ(FLEXCOMM7_IRQn);
	EnableIRQ(FLEXCOMM6_IRQn);

	// Initialize the I2S interfaces because they have to share the clock signal and the WS signal
	SYSCTL_Init(SYSCTL);
	/* select signal source for share set */
	SYSCTL_SetShareSignalSrc(SYSCTL, kSYSCTL_ShareSet0, kSYSCTL_SharedCtrlSignalSCK, kSYSCTL_Flexcomm7);
	SYSCTL_SetShareSignalSrc(SYSCTL, kSYSCTL_ShareSet0, kSYSCTL_SharedCtrlSignalWS, kSYSCTL_Flexcomm7);
	/* select share set for special flexcomm signal */
	SYSCTL_SetShareSet(SYSCTL, kSYSCTL_Flexcomm7, kSYSCTL_FlexcommSignalSCK, kSYSCTL_ShareSet0);
	SYSCTL_SetShareSet(SYSCTL, kSYSCTL_Flexcomm7, kSYSCTL_FlexcommSignalWS, kSYSCTL_ShareSet0);
	SYSCTL_SetShareSet(SYSCTL, kSYSCTL_Flexcomm6, kSYSCTL_FlexcommSignalSCK, kSYSCTL_ShareSet0);
	SYSCTL_SetShareSet(SYSCTL, kSYSCTL_Flexcomm6, kSYSCTL_FlexcommSignalWS, kSYSCTL_ShareSet0);

	// Enable the MCLK output for the audio codec peripheral
	SYSCON->MCLKIO  = 1U;

	/* Setup Audio */
	if (WM8904_Init(&wm8904Handle, &wm8904Config) != kStatus_WM8904_Success)
	{
		return -1;
	}

	// val max 63
	if (WM8904_SetVolume(&wm8904Handle, 57, 57) != kStatus_Success)
	{
		return -1;
	}

	/*
	 * masterSlave = kI2S_MasterSlaveNormalSlave;
	 * mode = kI2S_ModeI2sClassic;
	 * rightLow = false;
	 * leftJust = false;
	 * pdmData = false;
	 * sckPol = false;
	 * wsPol = false;
	 * divider = 1;
	 * oneChannel = false;
	 * dataLength = 16;
	 * frameLength = 32;
	 * position = 0;
	 * watermark = 4;
	 * txEmptyZero = false;
	 * pack48 = true;
	 */
	I2S_RxGetDefaultConfig(&i2s_rx_config);
	i2s_rx_config.divider     = I2S_CLOCK_DIVIDER;
	i2s_rx_config.masterSlave = kI2S_MasterSlaveNormalSlave;

	I2S_TxGetDefaultConfig(&i2s_tx_config);
	i2s_tx_config.divider     = I2S_CLOCK_DIVIDER;
	i2s_tx_config.masterSlave = kI2S_MasterSlaveNormalMaster;

	I2S_RxInit(I2S6, &i2s_rx_config);
	I2S_TxInit(I2S7, &i2s_tx_config);

	return 0;
}

/* pcm_vol
 *   set WM8904 CODEC left and right output volume levels --> [0,100]
 *   0   --> 0  =  mute
 *   1   --> 1  = -57dB
 *   100 --> 57 =   0dB
 *****/
static int pcmvol[2]={57,57};		// [0] --> left, [1] --> right
int pcm_volume(float left, float right)
{
	pcmvol[0]=(int)(57.0f+20.0f*logf(left/100.0f)/logf(10.0f));
	if (pcmvol[0]<0) pcmvol[0]=0;
	if (pcmvol[0]>63) pcmvol[0]=63;

	pcmvol[1]=(int)(57.0f+20.0f*logf(right/100.0f)/logf(10.0f));
	if (pcmvol[1]<0) pcmvol[1]=0;
	if (pcmvol[1]>63) pcmvol[1]=63;

	if (WM8904_SetMute(&wm8904Handle, !pcmvol[0], !pcmvol[1]) != kStatus_Success)
		return 0;
	// val max 63
	if (WM8904_SetVolume(&wm8904Handle, pcmvol[0], pcmvol[1]) != kStatus_Success)
		return 0;
	return 1;
}

/* pcm_get_freq
 *   returns the current sampling frequency
 */
unsigned int pcm_get_smpl_freq(void)
{
	return sample_freq;
}

typedef struct {
	uint32_t				N, M, P;
	uint32_t				mclk_Hz;
	wm8904_sample_rate_t	smpl_rate;
} Pll0AudioConfig_t;

const Pll0AudioConfig_t pll0cfg[] = {
	{5, 128, 25,	8192000,kWM8904_SampleRate8kHz},		/* 8kHz */
	{125, 2646, 15,	11289600,kWM8904_SampleRate11025Hz},	/* 11.025kHz */
	{25, 576, 15,	12288000,kWM8904_SampleRate12kHz},		/* 12kHz */
	{25, 512, 10,	16384000,kWM8904_SampleRate16kHz},		/* 16kHz */
	{125, 3528, 10,	22579200,kWM8904_SampleRate22050Hz},	/* 22.05kHz */
	{25, 768, 10,	24576000,kWM8904_SampleRate24kHz},		/* 24kHz */
	{25, 512, 5,	32768000,kWM8904_SampleRate32kHz},		/* 32kHz */
	{125, 3528, 5,	45158400,kWM8904_SampleRate44100Hz},	/* 44.1kHz */
	{25, 768, 5,	49152000,kWM8904_SampleRate48kHz}		/* 48kHz */
};
/* pcm_get_freq
 *   returns the updated sampling frequency
 */
unsigned int pcm_set_smpl_freq(unsigned int fs)
{
	i2s_config_t i2s_rx_config;
	i2s_config_t i2s_tx_config;
	unsigned int selp, seli;
	int idx=0;
	switch (fs) {
	case 8000:	/* 8kHz */
		idx=0;
		break;
	case 11025:	/* 11.025kHz */
		idx=1;
		break;
	case 12000:	/* 12kHz */
		idx=2;
		break;
	case 16000:	/* 16kHz */
		idx=3;
		break;
	case 22050:	/* 22.05kHz */
		idx=4;
		break;
	case 24000:	/* 24kHz */
		idx=5;
		break;
	case 32000:	/* 32kHz */
		idx=6;
		break;
	case 44100:	/* 44.1kHz */
		idx=7;
		break;
	case 48000:	/* 48kHz */
		idx=8;
		break;
	default:
		return sample_freq;
	}
	sample_freq=fs;
    
    selp=pll0cfg[idx].M/4+1;selp=(selp<=31) ? selp : 31;
    if (pll0cfg[idx].M>=8000) {
    	seli=1;
    } else if (pll0cfg[idx].M>=122) {
    	seli=8000/pll0cfg[idx].M;
    } else {
    	seli=2*(pll0cfg[idx].
    	M/4)+3;
    }
    pll_setup_t pll0setup = {
        .pllctrl = SYSCON_PLL0CTRL_CLKEN_MASK | SYSCON_PLL0CTRL_SELI(seli) | SYSCON_PLL0CTRL_SELP(selp),
        .pllndec = SYSCON_PLL0NDEC_NDIV(pll0cfg[idx].N),
        .pllpdec = SYSCON_PLL0PDEC_PDIV(pll0cfg[idx].P),
        .pllsscg = {0x0U,(SYSCON_PLL0SSCG1_MDIV_EXT(pll0cfg[idx].M) | SYSCON_PLL0SSCG1_SEL_EXT_MASK)},
        .pllRate = pll0cfg[idx].mclk_Hz,
        .flags   =  PLL_SETUPFLAG_WAITLOCK
    };
    CLOCK_SetPLL0Freq(&pll0setup);
	
	/* update I2S clock configuration */
	I2S_RxGetDefaultConfig(&i2s_rx_config);
	i2s_rx_config.divider     = I2S_CLOCK_DIVIDER;
	i2s_rx_config.masterSlave = kI2S_MasterSlaveNormalSlave;

	I2S_TxGetDefaultConfig(&i2s_tx_config);
	i2s_tx_config.divider     = I2S_CLOCK_DIVIDER;
	i2s_tx_config.masterSlave = kI2S_MasterSlaveNormalMaster;

	I2S_RxInit(I2S6, &i2s_rx_config);
	I2S_TxInit(I2S7, &i2s_tx_config);
	
	/* update CODEC clock configuration */
	wm8904Config.format.sampleRate = pll0cfg[idx].smpl_rate;
	wm8904Config.mclk_HZ = pll0cfg[idx].mclk_Hz;
	WM8904_Init(&wm8904Handle, &wm8904Config);
	
	return sample_freq;
}

/* pcm_play
 *   use DMA0 channel 19 connected to I2S7_Tx to send samples to the audio
 *   CODEC until Escape key is pressed.
 *****/
static void pcm_play_cb(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData)
{
	pcm_tx_buf_done=1;
}

int pcm_play(real *data, int ch, int n)
{
    const float *pcm_ch0=(float*)data;
    const float *pcm_ch1=(ch>1) ? (float*)(data+n) : (float*)data;
	int i=0;
	int pcm_buf_id=0;

	codec_set_master(&wm8904Handle,0);		// set codec in slave mode

    DMA_Init(DMA0);
	DMA_EnableChannel(DMA0, 19);
	DMA_SetChannelPriority(DMA0, 19, kDMA_ChannelPriority3);
	DMA_CreateHandle(&s_DmaTxHandle, DMA0, 19);

    I2S_TxTransferCreateHandleDMA(I2S7, &s_TxHandle, &s_DmaTxHandle, pcm_play_cb, NULL);

	pcm_tx_buf_done=0;

	int16_t *d;
    // setup 2 buffers with SAMPLE_NB left and right channel samples.
	//   use left and right channels as ring buffers
    d=(int16_t*)pcm_tx_xfer[0].data;
    for (int k=0;k<SAMPLE_NB;k++) {
    	*d++ = (int16_t)(pcm_ch0[i]*32768.0);	// left channel sample
    	*d++ = (int16_t)(pcm_ch1[i]*32768.0);	// right channel sample
     	i = ((i+1)==n) ? 0 : i+1;
    }

    d=(int16_t*)pcm_tx_xfer[1].data;
    for (int k=0;k<SAMPLE_NB;k++) {
    	*d++ = (int16_t)(pcm_ch0[i]*32768.0);	// left channel sample
    	*d++ = (int16_t)(pcm_ch1[i]*32768.0);	// right channel sample
     	i = ((i+1)==n) ? 0 : i+1;
    }
   /* need to queue two transmit buffers so when the first one
     * finishes transfer, the other immediatelly starts */
    I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_tx_xfer[0]);
    I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_tx_xfer[1]);

    while (1) {
    	while (!pcm_tx_buf_done) { }
    	pcm_tx_buf_done=0;

    	if (user_break) {		// a key was pressed, so stop and quit
    		scan_t scan;
    		char c=sys_wait_key(&scan);
    		if (c=='+') {
    			pcmvol[0]=(pcmvol[0]<57) ? pcmvol[0]+1 : 57;
    			pcmvol[1]=(pcmvol[1]<57) ? pcmvol[1]+1 : 57;
    		} else if (c=='-') {
    			pcmvol[0]=(pcmvol[0]>0) ? pcmvol[0]-1 : 0;
    			pcmvol[1]=(pcmvol[1]>0) ? pcmvol[1]-1 : 0;
    		} else if (scan==enter) {
    			break;
    		} else {
    			continue;
    		}
    		WM8904_SetMute(&wm8904Handle, !pcmvol[0], !pcmvol[1]);
			WM8904_SetVolume(&wm8904Handle, pcmvol[0], pcmvol[1]);
			continue;
    	}

    	d=(int16_t*)pcm_tx_xfer[pcm_buf_id].data;
        for (int k=0;k<SAMPLE_NB;k++) {
        	*d++ = (int16_t)(pcm_ch0[i]*32768.0);	// left channel sample
        	*d++ = (int16_t)(pcm_ch1[i]*32768.0);	// right channel sample
         	i = ((i+1)==n) ? 0 : i+1;
        }
        I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_tx_xfer[pcm_buf_id]);
    	pcm_buf_id=!pcm_buf_id;
    }

    I2S_TransferAbortDMA(I2S7, &s_TxHandle);

	return n;
}


/* pcm_record
 *   use DMA0 to get a buffer of n samples (2 ways) from I2S6_Rx connected to
 *   the audio device input line
 *****/
static void pcm_rec_rx_cb(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData)
{
	pcm_rx_buf_done=1;
}

int pcm_rec(real *data, int n)
{
	int pcm_rec_buf_id=0;

    float *pcm_rec_ch0=data;
    float *pcm_rec_ch1=data+n;
	int i=0;

    pcm_rx_buf_done=0;

    // set audio codec in master mode (it generates the i2s clock)
    codec_set_master(&wm8904Handle, 1);

	DMA_Init(DMA0);
	DMA_EnableChannel(DMA0, 16);
	DMA_SetChannelPriority(DMA0, 16, kDMA_ChannelPriority2);
	DMA_CreateHandle(&s_DmaRxHandle, DMA0, 16);

	I2S_RxTransferCreateHandleDMA(I2S6, &s_RxHandle, &s_DmaRxHandle, pcm_rec_rx_cb, NULL);

    /* need to queue two receive buffers so when the first one is filled,
     * the other is immediately starts to be filled */
    I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_rx_xfer[0]);
    I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_rx_xfer[1]);

    while (i<n) {
    	while (!pcm_rx_buf_done) {}
    	pcm_rx_buf_done=0;
    	int16_t *d=(int16_t*)pcm_rx_xfer[pcm_rec_buf_id].data;
    	pcm_rec_buf_id=!pcm_rec_buf_id;
        for (int k=0;k<SAMPLE_NB;++k) {
    		pcm_rec_ch0[i+k]=(float)d[2*k]/32768.0;
    		pcm_rec_ch1[i+k]=(float)d[2*k+1]/32768.0;
    		if (i+k==n) break;
        }
        i+=SAMPLE_NB;
        if (i<n-SAMPLE_NB) {
        	I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_rx_xfer[pcm_rec_buf_id]);
        }
    }

	return 1;
}

#if 1
/* pcm_loop
 *   use DMA0 to get a buffer of n samples (2 ways) from I2S6_Rx connected to
 *   the audio device input line
 *****/

#define BUF_NB	2
#define NEXT_ID(id) (((id)+1)<BUF_NB ? (id)+1 : 0)

volatile int pcm_buf_for_rxqueue=NEXT_ID(1);

static void pcm_loop_rx_cb(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData)
{
	pcm_rx_buf_done=1;
	/* setup next buffer for reception */
   	I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_rx_xfer[pcm_buf_for_rxqueue]);
   	pcm_buf_for_rxqueue= NEXT_ID(pcm_buf_for_rxqueue);
}

static void pcm_loop_tx_cb(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData)
{
	pcm_tx_buf_done=1;
}

static void echo_cb(int16_t *in, int16_t *out, int n)
{
	for (int k=0;k<2*n;++k) {
		*out++=*in++;
	}
}

int pcm_loop(fn_cb fn)
{
	int pcm_buf_id=0;
	int n=0;				// number of processed samples

    pcm_rx_buf_done=0;

    // setup processing function
    fn_cb proc = fn ? fn : echo_cb;

    // set audio codec in master mode (it generates the i2s clock)
    codec_set_master(&wm8904Handle, 0);

	DMA_Init(DMA0);
	DMA_EnableChannel(DMA0, 16);
	DMA_SetChannelPriority(DMA0, 16, kDMA_ChannelPriority2);
	DMA_CreateHandle(&s_DmaRxHandle, DMA0, 16);
	DMA_EnableChannel(DMA0, 19);
	DMA_SetChannelPriority(DMA0, 19, kDMA_ChannelPriority3);
	DMA_CreateHandle(&s_DmaTxHandle, DMA0, 19);

	I2S_RxTransferCreateHandleDMA(I2S6, &s_RxHandle, &s_DmaRxHandle, pcm_loop_rx_cb, NULL);
    I2S_TxTransferCreateHandleDMA(I2S7, &s_TxHandle, &s_DmaTxHandle, pcm_loop_tx_cb, NULL);

    // set tx buffer empty
    for (int i=0; i<4*SAMPLE_NB; ++i) {
    	pcm_tx_buf[i]=0;
    }
    /* we need to queue two receive buffers so when the first one
     * is full, the other immediately is starting to be filled */
    I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_rx_xfer[0]);
    I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_rx_xfer[1]);
    I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_tx_xfer[0]);
    I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_tx_xfer[1]);

    while (1) {
    	while (!pcm_rx_buf_done && !user_break) {}
    	pcm_rx_buf_done=0;

    	if (user_break) {		// a key was pressed, so stop and quit
    		scan_t scan;
    		char c=sys_wait_key(&scan);
    		if (c=='+') {
    			pcmvol[0]=(pcmvol[0]<57) ? pcmvol[0]+1 : 57;
    			pcmvol[1]=(pcmvol[1]<57) ? pcmvol[1]+1 : 57;
    		} else if (c=='-') {
    			pcmvol[0]=(pcmvol[0]>0) ? pcmvol[0]-1 : 0;
    			pcmvol[1]=(pcmvol[1]>0) ? pcmvol[1]-1 : 0;
    		} else if (scan==enter) {
    			break;
    		} else {
    			continue;
    		}
    		WM8904_SetMute(&wm8904Handle, !pcmvol[0], !pcmvol[1]);
			WM8904_SetVolume(&wm8904Handle, pcmvol[0], pcmvol[1]);
			continue;
    	}
    	io_set(1);
       	/* process data here */
    	int16_t *din=(int16_t*)pcm_rx_xfer[pcm_buf_id].data;
    	int16_t *dout=(int16_t*)pcm_tx_xfer[pcm_buf_id].data;

    	// process data
    	proc(din,dout,SAMPLE_NB);

    	/* output processed data */
        I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_tx_xfer[pcm_buf_id]);
    	pcm_buf_id=NEXT_ID(pcm_buf_id);

       	io_set(0);
       	n+=SAMPLE_NB;
    }

    I2S_TransferAbortDMA(I2S6, &s_RxHandle);
    I2S_TransferAbortDMA(I2S7, &s_TxHandle);
	return n;
}
#else

/* pcm_loop
 *   use DMA0 to get a buffer of n samples (2 ways) from I2S6_Rx connected to
 *   the audio device input line
 *****/

static int16_t pcm_buf[SAMPLE_NB*6];		/* 3 buffers with 2 channels with SAMPLE_NB samples */
static i2s_transfer_t pcm_xfer[3] = {
	[0] = {
		.data     = (uint8_t*)pcm_buf,
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	},
	[1] = {
		.data     = (uint8_t*)(pcm_buf+2*SAMPLE_NB),
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	},
	[2] = {
		.data     = (uint8_t*)(pcm_buf+4*SAMPLE_NB),
		.dataSize = 2*SAMPLE_NB*sizeof(int16_t)
	}
};

#define BUF_NB	3
#define NEXT_ID(id) (((id)+1)<BUF_NB ? (id)+1 : 0)

volatile int pcm_buf_for_rxqueue=NEXT_ID(1);

static void pcm_loop_rx_cb(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData)
{
	pcm_rx_buf_done=1;
	/* setup next buffer for reception */
   	I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_xfer[pcm_buf_for_rxqueue]);
   	pcm_buf_for_rxqueue= NEXT_ID(pcm_buf_for_rxqueue);
}

static void pcm_loop_tx_cb(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData)
{
	pcm_tx_buf_done=1;
}

static void echo_cb(int16_t *in, int16_t *out, int n)
{
}

int pcm_loop(fn_cb fn)
{
	int pcm_buf_id=0;
	int n=0;				// number of processed samples

    pcm_rx_buf_done=0;

    // setup process function
    fn_cb proc = fn ? fn : echo_cb;

    // set audio codec in master mode (it generates the i2s clock)
    codec_set_master(&wm8904Handle, 0);

	DMA_Init(DMA0);
	DMA_EnableChannel(DMA0, 16);
	DMA_SetChannelPriority(DMA0, 16, kDMA_ChannelPriority2);
	DMA_CreateHandle(&s_DmaRxHandle, DMA0, 16);
	DMA_EnableChannel(DMA0, 19);
	DMA_SetChannelPriority(DMA0, 19, kDMA_ChannelPriority3);
	DMA_CreateHandle(&s_DmaTxHandle, DMA0, 19);

	I2S_RxTransferCreateHandleDMA(I2S6, &s_RxHandle, &s_DmaRxHandle, pcm_loop_rx_cb, NULL);
    I2S_TxTransferCreateHandleDMA(I2S7, &s_TxHandle, &s_DmaTxHandle, pcm_loop_tx_cb, NULL);

    // set tx buffer empty
    for (int i=0; i<6*SAMPLE_NB; ++i) {
    	pcm_buf[i]=0;
    }
    /* we need to queue two receive buffers so when the first one
     * is full, the other immediately is starting to be filled */
    I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_xfer[0]);
    I2S_RxTransferReceiveDMA(I2S6, &s_RxHandle, pcm_xfer[1]);
    I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_xfer[1]);
    I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_xfer[2]);

    while (1) {
    	while (!pcm_rx_buf_done && !user_break) {}
    	pcm_rx_buf_done=0;

    	if (user_break) {		// a key was pressed, so stop and quit
    		scan_t scan;
    		char c=sys_wait_key(&scan);
    		if (c=='+') {
    			pcmvol[0]=(pcmvol[0]<57) ? pcmvol[0]+1 : 57;
    			pcmvol[1]=(pcmvol[1]<57) ? pcmvol[1]+1 : 57;
    		} else if (c=='-') {
    			pcmvol[0]=(pcmvol[0]>0) ? pcmvol[0]-1 : 0;
    			pcmvol[1]=(pcmvol[1]>0) ? pcmvol[1]-1 : 0;
    		} else if (scan==enter) {
    			break;
    		} else {
    			continue;
    		}
    		WM8904_SetMute(&wm8904Handle, !pcmvol[0], !pcmvol[1]);
			WM8904_SetVolume(&wm8904Handle, pcmvol[0], pcmvol[1]);
			continue;
    	}

    	io_set(1);
       	/* process data here */
    	int16_t *din=(int16_t*)pcm_xfer[pcm_buf_id].data;
    	int16_t *dout=(int16_t*)pcm_xfer[pcm_buf_id].data;

    	// process data
    	proc(din,dout,SAMPLE_NB);

    	/* output processed data */
        I2S_TxTransferSendDMA(I2S7, &s_TxHandle, pcm_xfer[pcm_buf_id]);
    	pcm_buf_id=NEXT_ID(pcm_buf_id);

       	io_set(0);
       	n+=SAMPLE_NB;
    }

    I2S_TransferAbortDMA(I2S6, &s_RxHandle);
    I2S_TransferAbortDMA(I2S7, &s_TxHandle);
	return n;
}
#endif

#if 0
#define BIQUAD_MAX_STAGES		12

typedef struct {
	float	b[3];	/* numerator coeffs */
	float	a[3];	/* denominator coeffs */
	float	w[3];	/* internal state */
} biquad_t;

typedef struct {
	biquad_t	biquads[BIQUAD_MAX_STAGES];
	float		gain;
	int			num_stages;
} iir_filter_t;

iir_filter_t filt;

float iir_calc(iir_filter_t *f, float x)
{
	float y=x;
	biquad_t *bq=f->biquads;
	for (int i=0; i< f->num_stages; i++) {
		
		/* A COMPLETER */
		
		bq++;
	}
	return y*f->gain;
}

void iir_filter_cb(int16_t*in, int16_t *out, int n) {
	for (int k=0; k<n; k++) {
		out[2*k]=iir_calc(&filt, in[2*k]);
//		out[2*k]=in[2*k]>>1;
		out[2*k+1]=0;
	}
}

void pcm_biquad(real *b, real *a, int r, int c, real *n)
{
	if (r-1>BIQUAD_MAX_STAGES) {
		*n=(real)0;
		return;
	}
	filt.num_stages=r-1;
	filt.gain=*b; b+=3; a+=3;
	for (int i=0; i<r-1; i++) {
		
		/* A COMPLETER */
		
		filt.biquads[i].w[0]=0.0;
		filt.biquads[i].w[1]=0.0;
		filt.biquads[i].w[2]=0.0;
	}
	*n=(real)pcm_loop(iir_filter_cb);
}
#else
#define BIQUAD_MAX_STAGES		12
pq_biquad_cascade_df2_instance S;

pq_biquad_state_t state[BIQUAD_MAX_STAGES];

void iir_filter_cb(int16_t*in, int16_t *out, int n) {
	for (int k=0; k<n; k++) {
		in[k]=in[2*k];
    }

	PQ_BiquadCascadeDf2Fixed16(&S, in, out, n);
	
    for (int k=n-1; k>=0; k--) {
    	out[2*k]=out[k];
    	out[2*k+1]=0;
    }
}

void pcm_biquad(real *b, real *a, int r, int c, real *n)
{
	if (r-1>BIQUAD_MAX_STAGES) {
		*n=(real)0;
		return;
	}
	
    PQ_BiquadCascadeDf2Init(&S, r, (pq_biquad_state_t *)&state);
	for (int i=0; i<r; i++) {
		
		/* A COMPLETER */
		
		state[i].compreg=0;
	}
	*n=(real)pcm_loop(iir_filter_cb);
}

#endif
