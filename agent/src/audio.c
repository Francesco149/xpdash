#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_BUFFERS 4

static HWAVEIN g_hwi = NULL;
static WAVEHDR g_headers[NUM_BUFFERS];
static uint8_t *g_buffers[NUM_BUFFERS] = {NULL};
static audio_frame_cb g_cb = NULL;
static void *g_cb_userdata = NULL;
static volatile int g_running = 0;
static int g_what_u_hear_active = 0;

/* Programmatically find and select "What U Hear" MUX control across all mixers */
static int configure_what_u_hear(void) {
    UINT num_mixers = mixerGetNumDevs();
    for (UINT m = 0; m < num_mixers; m++) {
        HMIXER hmx = NULL;
        if (mixerOpen(&hmx, m, 0, 0, MIXER_OBJECTF_MIXER) != MMSYSERR_NOERROR) {
            continue;
        }

        MIXERCAPSA mc;
        if (mixerGetDevCapsA(m, &mc, sizeof(mc)) != MMSYSERR_NOERROR) {
            mixerClose(hmx);
            continue;
        }

        for (DWORD d = 0; d < mc.cDestinations; d++) {
            MIXERLINEA dstLine;
            memset(&dstLine, 0, sizeof(dstLine));
            dstLine.cbStruct = sizeof(dstLine);
            dstLine.dwDestination = d;

            if (mixerGetLineInfoA((HMIXEROBJ)hmx, &dstLine, MIXER_GETLINEINFOF_DESTINATION) != MMSYSERR_NOERROR) {
                continue;
            }

            // Only check recording destinations
            if (dstLine.dwComponentType != MIXERLINE_COMPONENTTYPE_DST_WAVEIN) {
                continue;
            }

            if (dstLine.cControls == 0) continue;

            MIXERLINECONTROLSA mlc;
            MIXERCONTROLA *controls = calloc(dstLine.cControls, sizeof(MIXERCONTROLA));
            if (!controls) continue;

            memset(&mlc, 0, sizeof(mlc));
            mlc.cbStruct = sizeof(mlc);
            mlc.dwLineID = dstLine.dwLineID;
            mlc.cControls = dstLine.cControls;
            mlc.cbmxctrl = sizeof(MIXERCONTROLA);
            mlc.pamxctrl = controls;

            if (mixerGetLineControlsA((HMIXEROBJ)hmx, &mlc, MIXER_GETLINECONTROLSF_ALL) == MMSYSERR_NOERROR) {
                for (DWORD c = 0; c < dstLine.cControls; c++) {
                    if ((controls[c].dwControlType & MIXERCONTROL_CT_CLASS_MASK) == MIXERCONTROL_CT_CLASS_LIST) {
                        DWORD numItems = controls[c].cMultipleItems;
                        if (numItems > 0) {
                            MIXERCONTROLDETAILS_LISTTEXTA *listText = calloc(numItems, sizeof(MIXERCONTROLDETAILS_LISTTEXTA));
                            MIXERCONTROLDETAILS_BOOLEAN *vals = calloc(numItems, sizeof(MIXERCONTROLDETAILS_BOOLEAN));
                            MIXERCONTROLDETAILS mcd;

                            memset(&mcd, 0, sizeof(mcd));
                            mcd.cbStruct = sizeof(mcd);
                            mcd.dwControlID = controls[c].dwControlID;
                            mcd.cChannels = 1;
                            mcd.cMultipleItems = numItems;
                            mcd.cbDetails = sizeof(MIXERCONTROLDETAILS_LISTTEXTA);
                            mcd.paDetails = listText;

                            if (mixerGetControlDetailsA((HMIXEROBJ)hmx, &mcd, MIXER_GETCONTROLDETAILSF_LISTTEXT) == MMSYSERR_NOERROR) {
                                int target_idx = -1;
                                for (DWORD i = 0; i < numItems; i++) {
                                    if (strstr(listText[i].szName, "What U Hear") ||
                                        strstr(listText[i].szName, "Stereo Mix") ||
                                        strstr(listText[i].szName, "Wave Out Mix")) {
                                        target_idx = (int)i;
                                        break;
                                    }
                                }

                                if (target_idx >= 0) {
                                    for (DWORD i = 0; i < numItems; i++) {
                                        vals[i].fValue = ((int)i == target_idx) ? 1 : 0;
                                    }
                                    mcd.cbDetails = sizeof(MIXERCONTROLDETAILS_BOOLEAN);
                                    mcd.paDetails = vals;
                                    if (mixerSetControlDetails((HMIXEROBJ)hmx, &mcd, MIXER_SETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR) {
                                        g_what_u_hear_active = 1;
                                    }
                                }
                            }
                            free(vals);
                            free(listText);
                        }
                    }
                }
            }
            free(controls);
        }
        mixerClose(hmx);
        if (g_what_u_hear_active) break;
    }

    return g_what_u_hear_active;
}

static void CALLBACK waveInProc(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                                DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    (void)hwi; (void)dwInstance; (void)dwParam2;
    if (uMsg == WIM_DATA && g_running) {
        WAVEHDR *hdr = (WAVEHDR *)dwParam1;
        if (hdr && hdr->dwBytesRecorded > 0 && g_cb) {
            uint32_t pts = timeGetTime();
            g_cb((const uint8_t *)hdr->lpData, hdr->dwBytesRecorded, pts, g_cb_userdata);
        }
        if (g_running && hdr) {
            waveInAddBuffer(g_hwi, hdr, sizeof(WAVEHDR));
        }
    }
}

int audio_init(audio_frame_cb callback, void *user_data) {
    g_cb = callback;
    g_cb_userdata = user_data;

    configure_what_u_hear();

    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = AUDIO_CHANNELS;
    wfx.nSamplesPerSec = AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    MMRESULT res = waveInOpen(&g_hwi, WAVE_MAPPER, &wfx, (DWORD_PTR)waveInProc, 0, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        return 0;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_buffers[i] = (uint8_t *)malloc(AUDIO_FRAME_BYTES);
        if (!g_buffers[i]) return 0;
        memset(&g_headers[i], 0, sizeof(WAVEHDR));
        g_headers[i].lpData = (char *)g_buffers[i];
        g_headers[i].dwBufferLength = AUDIO_FRAME_BYTES;
        waveInPrepareHeader(g_hwi, &g_headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_hwi, &g_headers[i], sizeof(WAVEHDR));
    }

    return 1;
}

int audio_start(void) {
    if (!g_hwi) return 0;
    g_running = 1;
    return (waveInStart(g_hwi) == MMSYSERR_NOERROR);
}

void audio_stop(void) {
    g_running = 0;
    if (g_hwi) {
        waveInStop(g_hwi);
        waveInReset(g_hwi);
    }
}

void audio_shutdown(void) {
    audio_stop();
    if (g_hwi) {
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (g_headers[i].lpData) {
                waveInUnprepareHeader(g_hwi, &g_headers[i], sizeof(WAVEHDR));
            }
            if (g_buffers[i]) {
                free(g_buffers[i]);
                g_buffers[i] = NULL;
            }
        }
        waveInClose(g_hwi);
        g_hwi = NULL;
    }
}

int audio_is_what_u_hear_active(void) {
    return g_what_u_hear_active;
}
