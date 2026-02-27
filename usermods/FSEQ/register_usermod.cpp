#include "usermod_fpp.h"
#include "usermod_fseq.h"
#include "wled.h"

UsermodFseq usermodFseq;
REGISTER_USERMOD(usermodFseq);

UsermodFPP usermodFpp;
REGISTER_USERMOD(usermodFpp);

UsermodSdCard sd_card;
REGISTER_USERMOD(sd_card);