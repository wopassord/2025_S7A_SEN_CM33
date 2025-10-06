#ifndef DSP_H
#define DSP_H

#include "calc.h"

/* filter */
header* mfilter (Calc *cc, header *hd);

/* FFT */
header* mfft (Calc *cc, header *hd);
header* mifft (Calc *cc, header *hd);

/* accelerometer */
header* maccel (Calc *cc, header *hd);

/* power quad */
header* mpqcos(Calc* cc, header* hd);
header* mpqfft(Calc* cc, header* hd);
header* mpqifft(Calc* cc, header* hd);

/* audio */
header* mpcmvol (Calc *cc, header *hd);
header* mpcmfreq0 (Calc *cc, header *hd);
header* mpcmfreq (Calc *cc, header *hd);
header* mpcmplay (Calc *cc, header *hd);
header* mpcmrec(Calc *cc, header *hd);
header* mpcmloop (Calc *cc, header *hd);

/* audio filters */
header* mpcmbiquad(Calc* cc, header* hd);

#endif
