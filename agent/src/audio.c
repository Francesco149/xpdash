#include "audio.h"
#include "log.h"
#include <windows.h>
#include <mmsystem.h>
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
static UINT g_selected_device_id = WAVE_MAPPER;
static char g_selected_device_name[128] = "WAVE_MAPPER (Default)";
/* Case-insensitive substring search helper */
static int stristr(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h = haystack, *n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                h++; n++;
            }
            if (!*n) return 1;
        }
    }
    return 0;
}

/* Enumerate devices and select best device (manual override from agent.ini or auto-priority) */
static UINT select_audio_device(char *out_name, size_t max_len, char *out_preferred_line, size_t line_max_len) {
    const char *agent_ini = "C:\\xpdash\\agent.ini";
    int ini_device_idx = GetPrivateProfileIntA("audio", "device_index", -1, agent_ini);
    char ini_device_name[128] = "";
    GetPrivateProfileStringA("audio", "device_name", "", ini_device_name, sizeof(ini_device_name), agent_ini);
    GetPrivateProfileStringA("audio", "mixer_line", "", out_preferred_line, (DWORD)line_max_len, agent_ini);

    UINT num_devs = waveInGetNumDevs();
    agent_log("audio: enumerating %u waveIn recording devices", num_devs);

    UINT chosen_id = WAVE_MAPPER;
    int chosen_by = 0; // 0=default, 1=index override, 2=name override, 3=Creative auto-heuristic

    for (UINT i = 0; i < num_devs; i++) {
        WAVEINCAPSA wic;
        if (waveInGetDevCapsA(i, &wic, sizeof(wic)) == MMSYSERR_NOERROR) {
            agent_log("audio:   device %u: '%s' (channels=%u, formats=0x%08lX)",
                      i, wic.szPname, wic.wChannels, (unsigned long)wic.dwFormats);

            // Priority 1: Manual Index Override
            if (ini_device_idx >= 0 && (UINT)ini_device_idx == i) {
                chosen_id = i;
                chosen_by = 1;
                snprintf(out_name, max_len, "%s (Device %u - Manual Index)", wic.szPname, i);
            }
            // Priority 2: Manual Name Substring Override
            else if (chosen_by < 2 && ini_device_name[0] != '\0' && stristr(wic.szPname, ini_device_name)) {
                chosen_id = i;
                chosen_by = 2;
                snprintf(out_name, max_len, "%s (Device %u - Manual Name)", wic.szPname, i);
            }
            // Priority 3: Automatic Creative / Sound Blaster Heuristic
            else if (chosen_by < 3 && (stristr(wic.szPname, "Audigy") ||
                                       stristr(wic.szPname, "Sound Blaster") ||
                                       stristr(wic.szPname, "Live!") ||
                                       stristr(wic.szPname, "Creative") ||
                                       stristr(wic.szPname, "EMU10K"))) {
                chosen_id = i;
                chosen_by = 3;
                snprintf(out_name, max_len, "%s (Device %u - Creative EAX Hardware)", wic.szPname, i);
            }
        }
    }

    if (chosen_by == 0) {
        snprintf(out_name, max_len, "WAVE_MAPPER (Windows Default Device)");
    }

    agent_log("audio: selected waveIn device: %s (id=%d, selection_rule=%s)",
              out_name, (int)chosen_id,
              (chosen_by == 1) ? "ini_device_index" :
              (chosen_by == 2) ? "ini_device_name" :
              (chosen_by == 3) ? "creative_eax_auto_priority" : "windows_default");

    return chosen_id;
}

/* Helper to probe and select recording line on a specific mixer handle */
static int try_configure_mixer(HMIXER hmx, const char *preferred_line) {
    MIXERCAPSA mc;
    if (mixerGetDevCapsA((UINT_PTR)hmx, &mc, sizeof(mc)) != MMSYSERR_NOERROR) {
        return 0;
    }

    for (DWORD d = 0; d < mc.cDestinations; d++) {
        MIXERLINEA dstLine;
        memset(&dstLine, 0, sizeof(dstLine));
        dstLine.cbStruct = sizeof(dstLine);
        dstLine.dwDestination = d;

        if (mixerGetLineInfoA((HMIXEROBJ)hmx, &dstLine, MIXER_GETLINEINFOF_DESTINATION) != MMSYSERR_NOERROR) {
            continue;
        }
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
                            const char *found_name = "Unknown";

                            // 1. Check user preferred line from agent.ini
                            if (preferred_line && preferred_line[0] != '\0') {
                                for (DWORD i = 0; i < numItems; i++) {
                                    if (stristr(listText[i].szName, preferred_line)) {
                                        target_idx = (int)i;
                                        found_name = listText[i].szName;
                                        break;
                                    }
                                }
                            }

                            // 2. Automatic loopback candidates
                            if (target_idx < 0) {
                                const char *candidates[] = {
                                    "What U Hear", "Stereo Mix", "Wave Out Mix",
                                    "Wave", "Sum", "Mono Mix"
                                };
                                for (size_t cand = 0; cand < sizeof(candidates)/sizeof(candidates[0]); cand++) {
                                    for (DWORD i = 0; i < numItems; i++) {
                                        if (stristr(listText[i].szName, candidates[cand])) {
                                            target_idx = (int)i;
                                            found_name = listText[i].szName;
                                            break;
                                        }
                                    }
                                    if (target_idx >= 0) break;
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
                                    agent_log("audio: enabled loopback line '%s' (item %d/%lu) on mixer '%s'",
                                              found_name, target_idx, (unsigned long)numItems, mc.szPname);
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
        if (g_what_u_hear_active) break;
    }
    return g_what_u_hear_active;
}

/* Programmatically find and select loopback recording control ("What U Hear" / "Stereo Mix") */
static int configure_what_u_hear(UINT chosen_device_id, const char *preferred_line) {
    g_what_u_hear_active = 0;

    // Method A: Open mixer associated with chosen waveIn device directly
    if (chosen_device_id != WAVE_MAPPER) {
        HMIXER hmx = NULL;
        if (mixerOpen(&hmx, chosen_device_id, 0, 0, MIXER_OBJECTF_WAVEIN) == MMSYSERR_NOERROR) {
            if (try_configure_mixer(hmx, preferred_line)) {
                mixerClose(hmx);
                return 1;
            }
            mixerClose(hmx);
        }
    }

    // Method B: Probe all available mixer devices
    UINT num_mixers = mixerGetNumDevs();
    for (UINT m = 0; m < num_mixers; m++) {
        HMIXER hmx = NULL;
        if (mixerOpen(&hmx, m, 0, 0, MIXER_OBJECTF_MIXER) != MMSYSERR_NOERROR) {
            continue;
        }
        if (try_configure_mixer(hmx, preferred_line)) {
            mixerClose(hmx);
            return 1;
        }
        mixerClose(hmx);
    }

    if (!g_what_u_hear_active) {
        agent_log("audio: warning: no 'What U Hear' or 'Stereo Mix' line found; capturing current default recording line");
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

    char preferred_line[128] = "";
    g_selected_device_id = select_audio_device(g_selected_device_name, sizeof(g_selected_device_name),
                                               preferred_line, sizeof(preferred_line));

    configure_what_u_hear(g_selected_device_id, preferred_line);

    WAVEFORMATEX wfx;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = AUDIO_CHANNELS;
    wfx.nSamplesPerSec = AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    MMRESULT res = waveInOpen(&g_hwi, g_selected_device_id, &wfx, (DWORD_PTR)waveInProc, 0, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR && g_selected_device_id != WAVE_MAPPER) {
        agent_log("audio: waveInOpen failed on device %u (err=%u), falling back to WAVE_MAPPER",
                  g_selected_device_id, res);
        g_selected_device_id = WAVE_MAPPER;
        res = waveInOpen(&g_hwi, WAVE_MAPPER, &wfx, (DWORD_PTR)waveInProc, 0, CALLBACK_FUNCTION);
    }
    if (res != MMSYSERR_NOERROR) {
        agent_log("audio: waveInOpen failed (err=%u)!", res);
        return 0;
    }
    agent_log("audio: waveInOpen successfully opened device %s (hwi=%p)", g_selected_device_name, g_hwi);
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
