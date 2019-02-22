/*---------------------------------------------------------------------------*\

  FILE........: tofdm.c
  AUTHORS.....: David Rowe & Steve Sampson
  DATE CREATED: June 2017

  Tests for the C version of the OFDM modem.  This program
  outputs a file of Octave vectors that are loaded and automatically
  tested against the Octave version of the modem by the Octave script
  tofdm.m

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2017 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <complex.h>

#include "ofdm_internal.h"
#include "codec2_ofdm.h"
#include "octave.h"
#include "test_bits_ofdm.h"
#include "comp_prim.h"
#include "mpdecode_core.h"

#include "HRA_112_112.h"          /* generated by ldpc_fsk_lib.m:ldpc_decode() */

#define NFRAMES                   10
#define SAMPLE_CLOCK_OFFSET_PPM 100
#define FOFF_HZ                 0.5f

#define ASCALE  (2E5f * 1.1491f / 2.0f)  /* scale from shorts back to floats */

#define CODED_BITSPERFRAME 224    /* number of LDPC codeword bits/frame   */

/* QPSK constellation for symbol likelihood calculations */

static COMP S_matrix[] = {
    { 1.0f,  0.0f},
    { 0.0f,  1.0f},
    { 0.0f, -1.0f},
    {-1.0f,  0.0f}
};
         
/* static variables */

static struct OFDM *ofdm;
static struct OFDM_CONFIG *ofdm_config;

static float ofdm_tx_centre;        /* TX Center frequency */
static float ofdm_rx_centre;        /* RX Center frequency */
static float ofdm_fs;               /* Sample rate */
static float ofdm_ts;               /* Symbol cycle time */
static float ofdm_rs;               /* Symbol rate */
static float ofdm_tcp;              /* Cyclic prefix duration */
static float ofdm_timing_mx_thresh; /* See 700D Part 4 Acquisition blog post and ofdm_dev.m routines for how this was set */

static int ofdm_nc;                 /* NS-1 data symbols between pilots  */
static int ofdm_ns;
static int ofdm_bps;                /* Bits per symbol */
static int ofdm_m;                  /* duration of each symbol in samples */
static int ofdm_ncp;                /* duration of CP in samples */

static int ofdm_ftwindowwidth;
static int ofdm_bitsperframe;
static int ofdm_rowsperframe;
static int ofdm_samplesperframe;
static int ofdm_max_samplesperframe;
static int ofdm_rxbuf;
static int ofdm_ntxtbits;           /* reserve bits/frame for auxillary text information */
static int ofdm_nuwbits;            /* Unique word, used for positive indication of lock */

/*---------------------------------------------------------------------------*\

  FUNCTION....: fs_offset()
  AUTHOR......: David Rowe
  DATE CREATED: May 2015

  Simulates small Fs offset between mod and demod.
  (Note: Won't work with float, works OK with double)

\*---------------------------------------------------------------------------*/

static int fs_offset(COMP out[], COMP in[], int n, float sample_rate_ppm) {
    double f;
    double tin = 0.0;
    int t1, t2;
    int tout = 0;

    while (tin < (double) (n-1)) {
      t1 = (int) floor(tin);
      t2 = (int) ceil(tin);
      assert(t2 < n);

      f = (tin - (double) t1);

      out[tout].real = (1.0 - f) * in[t1].real + f * in[t2].real;
      out[tout].imag = (1.0 - f) * in[t1].imag + f * in[t2].imag;

      tout += 1;
      tin  += 1.0 + sample_rate_ppm / 1E6;
    }
    //printf("n: %d tout: %d tin: %f\n", n, tout, tin);
    
    return tout;
}

/*---------------------------------------------------------------------------*\

  FUNCTION....: freq_shift()
  AUTHOR......: David Rowe
  DATE CREATED: 26/4/2012

  Frequency shift modem signal.  The use of complex input and output allows
  single sided frequency shifting (no images).

\*---------------------------------------------------------------------------*/

static void freq_shift(COMP rx_fdm_fcorr[], COMP rx_fdm[], float foff, COMP *foff_phase_rect, int nin) {
    float temp = (TAU * foff / ofdm_fs);
    COMP  foff_rect = { COSF(temp), SINF(temp) };
    int   i;

    for (i = 0; i < nin; i++) {
	*foff_phase_rect = cmult(*foff_phase_rect, foff_rect);
	rx_fdm_fcorr[i] = cmult(rx_fdm[i], *foff_phase_rect);
    }

    /* normalise digital oscillator as the magnitude can drift over time */

    float mag = cabsolute(*foff_phase_rect);
    foff_phase_rect->real /= mag;
    foff_phase_rect->imag /= mag;
}

int main(int argc, char *argv[])
{
    if ((ofdm_config = (struct OFDM_CONFIG *) calloc(1, sizeof (struct OFDM_CONFIG))) == NULL) {
        fprintf(stderr, "Out of Memory\n");
        exit(1);
    }

    int ldpc_enable = 1;
    if (argc == 2)
        if (!strcmp(argv[1],"--noldpc")) {
            ldpc_enable = 0;
            fprintf(stderr, "LDPC disabled\n");
        }
    
    ofdm = ofdm_create(ofdm_config);
    assert(ofdm != NULL);

    free(ofdm_config);

    /* Get a copy of the actual modem config */
    ofdm_config = ofdm_get_config_param();

    ofdm_tx_centre = ofdm_config->tx_centre;
    ofdm_rx_centre = ofdm_config->rx_centre;
    ofdm_fs = ofdm_config->fs;
    ofdm_ts = ofdm_config->ts;
    ofdm_rs = ofdm_config->rs;
    ofdm_tcp = ofdm_config->tcp;
    ofdm_timing_mx_thresh = ofdm_config->ofdm_timing_mx_thresh;
    ofdm_nc = ofdm_config->nc;
    ofdm_ns = ofdm_config->ns;
    ofdm_bps = ofdm_config->bps;
    ofdm_m = (int) (ofdm_config->fs / ofdm_config->rs);
    ofdm_ncp = (int) (ofdm_config->tcp * ofdm_config->fs);
    ofdm_ftwindowwidth = ofdm_config->ftwindowwidth;
    ofdm_bitsperframe = ofdm_get_bits_per_frame();
    ofdm_rowsperframe = ofdm_bitsperframe / (ofdm_config->nc * ofdm_config->bps);
    ofdm_samplesperframe = ofdm_get_samples_per_frame();
    ofdm_max_samplesperframe = ofdm_get_max_samples_per_frame();
    ofdm_rxbuf = 3 * ofdm_samplesperframe + 3 * (ofdm_m + ofdm_ncp);
    ofdm_ntxtbits = ofdm_config->txtbits;
    ofdm_nuwbits = (ofdm_config->ns - 1) * ofdm_config->bps - ofdm_config->txtbits;

    int tx_bits[ofdm_samplesperframe];
    COMP tx[ofdm_samplesperframe];         /* one frame of tx samples */

    int rx_bits[ofdm_bitsperframe];    /* one frame of rx bits    */

    /* log arrays */

    int tx_bits_log[ofdm_bitsperframe*NFRAMES];
    COMP tx_log[ofdm_samplesperframe*NFRAMES];
    COMP rx_log[ofdm_samplesperframe*NFRAMES];
    COMP rxbuf_in_log[ofdm_max_samplesperframe*NFRAMES];
    COMP rxbuf_log[ofdm_rxbuf*NFRAMES];
    COMP rx_sym_log[(ofdm_ns + 3)*NFRAMES][ofdm_nc + 2];
    float phase_est_pilot_log[ofdm_rowsperframe*NFRAMES][ofdm_nc];
    COMP rx_np_log[ofdm_rowsperframe*ofdm_nc*NFRAMES];
    float rx_amp_log[ofdm_rowsperframe*ofdm_nc*NFRAMES];
    float foff_hz_log[NFRAMES];
    int rx_bits_log[ofdm_bitsperframe*NFRAMES];
    int timing_est_log[NFRAMES];
    int timing_valid_log[NFRAMES];
    float timing_mx_log[NFRAMES];
    float coarse_foff_est_hz_log[NFRAMES];
    int sample_point_log[NFRAMES];
    float symbol_likelihood_log[ (CODED_BITSPERFRAME/ofdm_bps) * (1<<ofdm_bps) * NFRAMES];
    float bit_likelihood_log[CODED_BITSPERFRAME * NFRAMES];        
    int detected_data_log[CODED_BITSPERFRAME * NFRAMES];
    float sig_var_log[NFRAMES], noise_var_log[NFRAMES];        
    float mean_amp_log[NFRAMES];        
    
    FILE *fout;
    int f,i,j;

    /* set up LDPC code */
    
    struct LDPC   ldpc;

    ldpc.max_iter = HRA_112_112_MAX_ITER;
    ldpc.dec_type = 0;
    ldpc.q_scale_factor = 1;
    ldpc.r_scale_factor = 1;
    ldpc.CodeLength = HRA_112_112_CODELENGTH;
    ldpc.NumberParityBits = HRA_112_112_NUMBERPARITYBITS;
    ldpc.NumberRowsHcols = HRA_112_112_NUMBERROWSHCOLS;
    ldpc.max_row_weight = HRA_112_112_MAX_ROW_WEIGHT;
    ldpc.max_col_weight = HRA_112_112_MAX_COL_WEIGHT;
    ldpc.H_rows = (uint16_t *)HRA_112_112_H_rows;
    ldpc.H_cols = (uint16_t *)HRA_112_112_H_cols;

    /* Main Loop ---------------------------------------------------------------------*/

    for(f=0; f<NFRAMES; f++) {

	/* --------------------------------------------------------*\
	                          Mod
	\*---------------------------------------------------------*/

        /* See CML startup code in tofdm.m */

        for(i=0; i<ofdm_nuwbits; i++) {
            tx_bits[i] = ofdm->tx_uw[i];
        }
        for(i=ofdm_nuwbits; i<ofdm_nuwbits+ofdm_ntxtbits; i++) {
            tx_bits[i] = 0;
        }       

        if (ldpc_enable) {
            unsigned char ibits[HRA_112_112_NUMBERROWSHCOLS];
            unsigned char pbits[HRA_112_112_NUMBERPARITYBITS];

            assert(HRA_112_112_NUMBERROWSHCOLS == ldpc.CodeLength/2);
            for(i=0; i<ldpc.CodeLength/2; i++) {
                ibits[i] = payload_data_bits[i];
            }
            encode(&ldpc, ibits, pbits);
            for(j=0, i=ofdm_nuwbits+ofdm_ntxtbits; j<ldpc.CodeLength/2; i++,j++) {
                tx_bits[i] = ibits[j];
            }
            for(j=0; j<ldpc.CodeLength/2; i++,j++) {
                tx_bits[i] = pbits[j];
            }
            assert(i == ofdm_bitsperframe);
        } else {
            for(i=ofdm_nuwbits+ofdm_ntxtbits,j=0; j<ldpc.CodeLength/2; i++,j++) {
                tx_bits[i] = payload_data_bits[j];
            }
            for(j=0; j<ldpc.CodeLength/2; i++,j++) {
                tx_bits[i] = payload_data_bits[j];
            }
        }
        
        ofdm_mod(ofdm, (COMP*)tx, tx_bits);
        
        /* tx vector logging */

	memcpy(&tx_bits_log[ofdm_bitsperframe*f], tx_bits, sizeof(int)*ofdm_bitsperframe);
	memcpy(&tx_log[ofdm_samplesperframe*f], tx, sizeof(COMP)*ofdm_samplesperframe);
    }

    /* --------------------------------------------------------*\
	                        Channel
    \*---------------------------------------------------------*/

    fs_offset(rx_log, tx_log, ofdm_samplesperframe*NFRAMES, SAMPLE_CLOCK_OFFSET_PPM);

    COMP foff_phase_rect = {1.0f, 0.0f};

    freq_shift(rx_log, rx_log, FOFF_HZ, &foff_phase_rect, ofdm_samplesperframe * NFRAMES);

    /* --------------------------------------------------------*\
	                        Demod
    \*---------------------------------------------------------*/

    /* Init/pre-load rx with ideal timing so we can test with timing estimation disabled */

    int Nsam = ofdm_samplesperframe*NFRAMES;
    int prx = 0;
    int nin = ofdm_samplesperframe + 2*(ofdm_m+ofdm_ncp);

    int  lnew;
    COMP rxbuf_in[ofdm_max_samplesperframe];

#define FRONT_LOAD
#ifdef FRONT_LOAD
    for (i=0; i<nin; i++,prx++) {
         ofdm->rxbuf[ofdm_rxbuf-nin+i] = rx_log[prx].real + rx_log[prx].imag * I;
    }
#endif
    
    int nin_tot = 0;

    /* disable estimators for initial testing */

    ofdm_set_verbose(ofdm, false);
    ofdm_set_timing_enable(ofdm, true);
    ofdm_set_foff_est_enable(ofdm, true);
    ofdm_set_phase_est_enable(ofdm, true);

//#define TESTING_FILE
#ifdef TESTING_FILE
    FILE *fin=fopen("~/codec2-dev/octave/ofdm_test.raw", "rb");
    assert(fin != NULL);
    int Nbitsperframe = ofdm_bitsperframe;
    int Nmaxsamperframe = ofdm_max_samplesperframe;
    short rx_scaled[Nmaxsamperframe];
#endif

    /* start this with something sensible otherwise LDPC decode fails in tofdm.m */

    ofdm->mean_amp = 1.0;
       
    for(f=0; f<NFRAMES; f++) {
        /* For initial testing, timing est is off, so nin is always
           fixed.  TODO: we need a constant for rxbuf_in[] size that
           is the maximum possible nin */

        nin = ofdm_get_nin(ofdm);
        assert(nin <= ofdm_max_samplesperframe);

        /* Insert samples at end of buffer, set to zero if no samples
           available to disable phase estimation on future pilots on
           last frame of simulation. */

        if ((Nsam-prx) < nin) {
            lnew = Nsam-prx;
        } else {
            lnew = nin;
        }
        //printf("nin: %d prx: %d lnew: %d\n", nin, prx, lnew);
        for(i=0; i<nin; i++) {
            rxbuf_in[i].real = 0.0;
            rxbuf_in[i].imag = 0.0;
        }

        if (lnew) {
            for(i=0; i<lnew; i++, prx++) {
                rxbuf_in[i] = rx_log[prx];
            }
        }
        assert(prx <= ofdm_max_samplesperframe*NFRAMES);

#ifdef TESTING_FILE
        fread(rx_scaled, sizeof(short), nin, fin);

        for(i=0; i<nin; i++) {
	    rxbuf_in[i].real = (float)rx_scaled[i]/ASCALE;
            rxbuf_in[i].imag = 0.0;
        }
#endif

        /* uncoded OFDM modem ---------------------------------------*/
        
        ofdm_demod(ofdm, rx_bits, rxbuf_in);
        
#ifdef TESTING_FILE
        int Nerrs = 0;
        for(i=0; i<Nbitsperframe; i++) {
            if (test_bits_ofdm[i] != rx_bits[i]) {
                Nerrs++;
            }
        }
        printf("f: %d Nerr: %d\n", f, Nerrs);
#endif
        
        /* LDPC functions --------------------------------------*/

        float symbol_likelihood[ (CODED_BITSPERFRAME/ofdm_bps) * (1<<ofdm_bps) ];
        float bit_likelihood[CODED_BITSPERFRAME];
        float EsNo = 10;
        
        /* first few symbols are used for UW and txt bits, find start of (224,112) LDPC codeword */

        assert((ofdm_nuwbits+ofdm_ntxtbits+CODED_BITSPERFRAME) == ofdm_bitsperframe);

        COMP ldpc_codeword_symbols[(CODED_BITSPERFRAME/ofdm_bps)];

        for(i=0, j=(ofdm_nuwbits+ofdm_ntxtbits)/ofdm_bps; i<(CODED_BITSPERFRAME/ofdm_bps); i++,j++) {
            ldpc_codeword_symbols[i].real = crealf(ofdm->rx_np[j]);
            ldpc_codeword_symbols[i].imag = cimagf(ofdm->rx_np[j]);
        }

        float *ldpc_codeword_symbol_amps = &ofdm->rx_amp[(ofdm_nuwbits+ofdm_ntxtbits)/ofdm_bps];
                
        Demod2D(symbol_likelihood, ldpc_codeword_symbols, S_matrix, EsNo, ldpc_codeword_symbol_amps, ofdm->mean_amp, CODED_BITSPERFRAME/ofdm_bps);
        Somap(bit_likelihood, symbol_likelihood, CODED_BITSPERFRAME/ofdm_bps);

        float  llr[CODED_BITSPERFRAME];
        uint8_t out_char[CODED_BITSPERFRAME];
        int    parityCheckCount;
        
        
        // fprintf(stderr, "\n");
        for(i=0; i<CODED_BITSPERFRAME; i++) {
            llr[i] = -bit_likelihood[i];
            // fprintf(stderr, "%f ", llr[i]);
        }
        
        //fprintf(stderr, "\n");
        
        run_ldpc_decoder(&ldpc, out_char, llr, &parityCheckCount);
        /*
          fprintf(stderr, "iter: %d parityCheckCount: %d\n", iter, parityCheckCount);
        for(i=0; i<CODED_BITSPERFRAME; i++) {
            fprintf(stderr, "%d ", out_char[i]);
        }
        */
        
        /* rx vector logging -----------------------------------*/

        assert(nin_tot < ofdm_samplesperframe*NFRAMES);
	memcpy(&rxbuf_in_log[nin_tot], rxbuf_in, sizeof(COMP)*nin);
        nin_tot += nin;

        for(i=0; i<ofdm_rxbuf; i++) {
            rxbuf_log[ofdm_rxbuf*f+i].real = crealf(ofdm->rxbuf[i]);
            rxbuf_log[ofdm_rxbuf*f+i].imag = cimagf(ofdm->rxbuf[i]);
        }

        for (i = 0; i < (ofdm_ns + 3); i++) {
            for (j = 0; j < (ofdm_nc + 2); j++) {
                rx_sym_log[(ofdm_ns + 3)*f+i][j].real = crealf(ofdm->rx_sym[i][j]);
                rx_sym_log[(ofdm_ns + 3)*f+i][j].imag = cimagf(ofdm->rx_sym[i][j]);
            }
        }

        /* note corrected phase (rx no phase) is one big linear array for frame */

        for (i = 0; i < ofdm_rowsperframe*ofdm_nc; i++) {
            rx_np_log[ofdm_rowsperframe*ofdm_nc*f + i].real = crealf(ofdm->rx_np[i]);
            rx_np_log[ofdm_rowsperframe*ofdm_nc*f + i].imag = cimagf(ofdm->rx_np[i]);
        }

        /* note phase/amp ests the same for each col, but check them all anyway */

        for (i = 0; i < ofdm_rowsperframe; i++) {
            for (j = 0; j < ofdm_nc; j++) {
                phase_est_pilot_log[ofdm_rowsperframe*f+i][j] = ofdm->aphase_est_pilot_log[ofdm_nc*i+j];
                rx_amp_log[ofdm_rowsperframe*ofdm_nc*f+ofdm_nc*i+j] = ofdm->rx_amp[ofdm_nc*i+j];
            }
        }

        foff_hz_log[f] = ofdm->foff_est_hz;
        timing_est_log[f] = ofdm->timing_est + 1;     /* offset by 1 to match Octave */
        timing_valid_log[f] = ofdm->timing_valid;     
        timing_mx_log[f] = ofdm->timing_mx;           
        coarse_foff_est_hz_log[f] = ofdm->coarse_foff_est_hz;
        sample_point_log[f] = ofdm->sample_point + 1; /* offset by 1 to match Octave */
        sig_var_log[f] = ofdm->sig_var;
        noise_var_log[f] = ofdm->noise_var;
        mean_amp_log[f] = ofdm->mean_amp;

        memcpy(&rx_bits_log[ofdm_bitsperframe*f], rx_bits, sizeof(rx_bits));

        for(i=0; i<(CODED_BITSPERFRAME/ofdm_bps) * (1<<ofdm_bps); i++) {
            symbol_likelihood_log[ (CODED_BITSPERFRAME/ofdm_bps) * (1<<ofdm_bps) * f + i] = symbol_likelihood[i];
        }
        for(i=0; i<CODED_BITSPERFRAME; i++) {
            bit_likelihood_log[CODED_BITSPERFRAME*f + i] =  bit_likelihood[i];
            detected_data_log[CODED_BITSPERFRAME*f + i] = out_char[i];
        }
    }

    /*---------------------------------------------------------*\
               Dump logs to Octave file for evaluation
                      by tofdm.m Octave script
    \*---------------------------------------------------------*/

    fout = fopen("tofdm_out.txt","wt");
    assert(fout != NULL);
    fprintf(fout, "# Created by tofdm.c\n");
    octave_save_complex(fout, "pilot_samples_c", (COMP*)ofdm->pilot_samples, 1, ofdm_m+ofdm_ncp, ofdm_m+ofdm_ncp);
    octave_save_int(fout, "tx_bits_log_c", tx_bits_log, 1, ofdm_bitsperframe*NFRAMES);
    octave_save_complex(fout, "tx_log_c", (COMP*)tx_log, 1, ofdm_samplesperframe*NFRAMES,  ofdm_samplesperframe*NFRAMES);
    octave_save_complex(fout, "rx_log_c", (COMP*)rx_log, 1, ofdm_samplesperframe*NFRAMES,  ofdm_samplesperframe*NFRAMES);
    octave_save_complex(fout, "rxbuf_in_log_c", (COMP*)rxbuf_in_log, 1, nin_tot, nin_tot);
    octave_save_complex(fout, "rxbuf_log_c", (COMP*)rxbuf_log, 1, ofdm_rxbuf*NFRAMES,  ofdm_rxbuf*NFRAMES);
    octave_save_complex(fout, "rx_sym_log_c", (COMP*)rx_sym_log, (ofdm_ns + 3)*NFRAMES, ofdm_nc + 2, ofdm_nc + 2);
    octave_save_float(fout, "phase_est_pilot_log_c", (float*)phase_est_pilot_log, ofdm_rowsperframe*NFRAMES, ofdm_nc, ofdm_nc);
    octave_save_float(fout, "rx_amp_log_c", (float*)rx_amp_log, 1, ofdm_rowsperframe*ofdm_nc*NFRAMES, ofdm_rowsperframe*ofdm_nc*NFRAMES);
    octave_save_float(fout, "foff_hz_log_c", foff_hz_log, NFRAMES, 1, 1);
    octave_save_int(fout, "timing_est_log_c", timing_est_log, NFRAMES, 1);
    octave_save_int(fout, "timing_valid_log_c", timing_valid_log, NFRAMES, 1);
    octave_save_float(fout, "timing_mx_log_c", timing_mx_log, NFRAMES, 1, 1);
    octave_save_float(fout, "coarse_foff_est_hz_log_c", coarse_foff_est_hz_log, NFRAMES, 1, 1);
    octave_save_int(fout, "sample_point_log_c", sample_point_log, NFRAMES, 1);
    octave_save_complex(fout, "rx_np_log_c", (COMP*)rx_np_log, 1, ofdm_rowsperframe*ofdm_nc*NFRAMES, ofdm_rowsperframe*ofdm_nc*NFRAMES);
    octave_save_int(fout, "rx_bits_log_c", rx_bits_log, 1, ofdm_bitsperframe*NFRAMES);
    octave_save_float(fout, "symbol_likelihood_log_c", symbol_likelihood_log, (CODED_BITSPERFRAME/ofdm_bps) * (1<<ofdm_bps) * NFRAMES, 1, 1);
    octave_save_float(fout, "bit_likelihood_log_c", bit_likelihood_log, CODED_BITSPERFRAME * NFRAMES, 1, 1);
    octave_save_int(fout, "detected_data_log_c", detected_data_log, 1, CODED_BITSPERFRAME*NFRAMES);
    octave_save_float(fout, "sig_var_log_c", sig_var_log, NFRAMES, 1, 1);
    octave_save_float(fout, "noise_var_log_c", noise_var_log, NFRAMES, 1, 1);
    octave_save_float(fout, "mean_amp_log_c", mean_amp_log, NFRAMES, 1, 1);
    fclose(fout);
#ifdef TESTING_FILE
    fclose(fin);
#endif

    ofdm_destroy(ofdm);

    return 0;
}

