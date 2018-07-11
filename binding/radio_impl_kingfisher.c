/*
 * Copyright (C) 2017,2018 Konsulko Group
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <gst/gst.h>
#include <afb/afb-binding.h>

#include "radio_impl.h"

#define SI_NODE	"/sys/firmware/devicetree/base/si468x@0/compatible"
#define SI_INIT	"/usr/bin/si_init"
#define SI_CTL	"/usr/bin/si_ctl"

#define GST_SINK_OPT_LEN	128
#define GST_PIPELINE_LEN	256

// Structure to describe FM band plans, all values in Hz.
typedef struct {
	char *name;
	uint32_t min;
	uint32_t max;
	uint32_t step;
} fm_band_plan_t;

static fm_band_plan_t known_fm_band_plans[5] = {
	{ .name = "US", .min = 87900000, .max = 107900000, .step = 200000 },
	{ .name = "JP", .min = 76000000, .max = 95000000, .step = 100000 },
	{ .name = "EU", .min = 87500000, .max = 108000000, .step = 50000 },
	{ .name = "ITU-1", .min = 87500000, .max = 108000000, .step = 50000 },
	{ .name = "ITU-2", .min = 87900000, .max = 107900000, .step = 50000 }
};

static unsigned int bandplan = 0;
static bool present;
static uint32_t current_frequency;
static int scan_valid_snr_threshold = 128;
static int scan_valid_rssi_threshold = 128;
static bool scanning = false;

// GStreamer state
static GstElement *pipeline;
static bool running;

static void (*freq_callback)(uint32_t, void*);
static void *freq_callback_data;

static uint32_t kf_get_min_frequency(radio_band_t band);
static void kf_scan_stop(void);

static int kf_init(const char *output)
{
	GKeyFile* conf_file;
	int conf_file_present = 0;
	struct stat statbuf;
	char *value_str;
	char cmd[128];
	int rc;
	char output_sink_opt[GST_SINK_OPT_LEN];
	char gst_pipeline_str[GST_PIPELINE_LEN];

	if(present)
		return 0;

	// Check for Kingfisher SI486x devicetree node
	if(stat(SI_NODE, &statbuf) != 0)
		return -1;

	// Check for Cogent's si_init script and si_ctl utility
	if(stat(SI_INIT, &statbuf) != 0)
		return -1;
	if(stat(SI_CTL, &statbuf) != 0)
		return -1;

	// Load settings from configuration file if it exists
	conf_file = g_key_file_new();
	if(conf_file &&
	   g_key_file_load_from_dirs(conf_file,
				     "AGL.conf",
				     (const gchar**) g_get_system_config_dirs(),
				     NULL,
				     G_KEY_FILE_KEEP_COMMENTS,
				     NULL) == TRUE) {
		conf_file_present = 1;

		// Set band plan if it is specified
		value_str = g_key_file_get_string(conf_file,
						  "radio",
						  "fmbandplan",
						  NULL);
		if(value_str) {
			unsigned int i;
			for(i = 0;
			    i < sizeof(known_fm_band_plans) / sizeof(fm_band_plan_t);
			    i++) {
				if(!strcasecmp(value_str, known_fm_band_plans[i].name)) {
					bandplan = i;
					break;
				}
			}
		}
	}

	if(conf_file_present) {
		GError *error = NULL;
		int n;

		// Allow over-riding scanning parameters just in case a demo
		// setup needs to do so to work reliably.
		n = g_key_file_get_integer(conf_file,
					   "radio",
					   "scan_valid_snr_threshold",
					   &error);
		if(!error) {
			AFB_INFO("Scan valid SNR level set to %d", n);
			scan_valid_snr_threshold = n;
		}

		error = NULL;
		n = g_key_file_get_integer(conf_file,
					   "radio",
					   "scan_valid_rssi_threshold",
					   &error);
		if(!error) {
			AFB_INFO("Scan valid SNR level set to %d", n);
			scan_valid_rssi_threshold = n;
		}

		g_key_file_free(conf_file);
	}

	rc = system(SI_INIT);
	if(rc != 0) {
		AFB_ERROR("si_init failed, rc = %d", rc);
		return -1;
	}

	AFB_INFO("Using FM Bandplan: %s", known_fm_band_plans[bandplan].name);
	current_frequency = kf_get_min_frequency(BAND_FM);
	sprintf(cmd,
		"%s /dev/i2c-12 0x65 -b fm -p %s -t %d -u %d -c %d",
		SI_CTL,
		known_fm_band_plans[bandplan].name,
		scan_valid_snr_threshold,
		scan_valid_rssi_threshold,
		current_frequency / 1000);
	rc = system(cmd);
	if(rc != 0) {
		AFB_ERROR("%s failed, rc = %d", SI_CTL, rc);
		return -1;
	}

	// Initialize GStreamer
	gst_init(NULL, NULL);

	if(output) {
		AFB_INFO("Using output device %s", output);
		rc = snprintf(output_sink_opt, GST_SINK_OPT_LEN, " device=%s", output);
		if(rc >= GST_SINK_OPT_LEN) {
			AFB_ERROR("output device string too long");
			return -1;
		}
	} else {
		output_sink_opt[0] = '\0';
	}
	// NOTE: If muting without pausing is desired, it can likely be done
	//       by adding a "volume" element to the pipeline before the sink,
	//       and setting the volume to 0 to mute.
#ifdef HAVE_4A_FRAMEWORK
	rc = snprintf(gst_pipeline_str,
		      GST_PIPELINE_LEN,
		      "alsasrc device=hw:radio ! queue ! audioconvert ! audioresample ! alsasink%s",
		      output_sink_opt);
#else
	// Use PulseAudio output for compatibility with audiomanager / module_router
	rc = snprintf(gst_pipeline_str,
		      GST_PIPELINE_LEN,
		      "alsasrc device=hw:radio ! queue ! audioconvert ! audioresample ! pulsesink stream-properties=\"props,media.role=radio\"");
#endif
	if(rc >= GST_PIPELINE_LEN) {
		AFB_ERROR("pipeline string too long");
		return -1;
	}
	pipeline = gst_parse_launch(gst_pipeline_str, NULL);
	if(!pipeline) {
		AFB_ERROR("pipeline construction failed!");
		return -1;
	}

	// Start pipeline in paused state
	gst_element_set_state(pipeline, GST_STATE_PAUSED);

	present = true;
	return 0;
}

static uint32_t kf_get_frequency(void)
{
	return current_frequency;
}

static void kf_set_frequency(uint32_t frequency)
{
	char cmd[128];
	int rc;

	if(!present)
		return;

	if(scanning)
		return;

	if(frequency < known_fm_band_plans[bandplan].min ||
	   frequency > known_fm_band_plans[bandplan].max)
		return;

	kf_scan_stop();
	sprintf(cmd, "%s /dev/i2c-12 0x65 -c %d", SI_CTL, frequency / 1000);
	rc = system(cmd);
	if(rc == 0)
		current_frequency = frequency;

	if(freq_callback)
		freq_callback(current_frequency, freq_callback_data);
}

static void kf_set_frequency_callback(radio_freq_callback_t callback,
				      void *data)
{
	freq_callback = callback;
	freq_callback_data = data;
}

static radio_band_t kf_get_band(void)
{
	return BAND_FM;
}

static void kf_set_band(radio_band_t band)
{
	// We only support FM, so do nothing
}

static int kf_band_supported(radio_band_t band)
{
	if(band == BAND_FM)
		return 1;
	return 0;
}

static uint32_t kf_get_min_frequency(radio_band_t band)
{
	return known_fm_band_plans[bandplan].min;
}

static uint32_t kf_get_max_frequency(radio_band_t band)
{
	return known_fm_band_plans[bandplan].max;
}

static uint32_t kf_get_frequency_step(radio_band_t band)
{
	uint32_t ret = 0;

	switch (band) {
	case BAND_AM:
		ret = 1000; // 1 kHz
		break;
	case BAND_FM:
		ret = known_fm_band_plans[bandplan].step;
		break;
	default:
		break;
	}
	return ret;
}

static void kf_start(void)
{
	if(!present)
		return;

	if(!running) {
		// Start pipeline
		gst_element_set_state(pipeline, GST_STATE_PLAYING);
		running = true;
	}
}

static void kf_stop(void)
{
	GstEvent *event;

	if(present && running) {
		// Stop pipeline
		running = false;
		gst_element_set_state(pipeline, GST_STATE_PAUSED);

		// Flush pipeline
		// This seems required to avoid stutters on starts after a stop
		event = gst_event_new_flush_start();
		gst_element_send_event(GST_ELEMENT(pipeline), event);
		event = gst_event_new_flush_stop(TRUE);
		gst_element_send_event(GST_ELEMENT(pipeline), event); 
	}
}

static void kf_scan_start(radio_scan_direction_t direction,
			  radio_scan_callback_t callback,
			  void *data)
{
	int rc;
	char cmd[128];
	char line[128];
	uint32_t new_frequency = 0;
	FILE *fp;

	if(!present)
		return;

	if(scanning)
		return;

	scanning = true;
	sprintf(cmd, "%s /dev/i2c-12 0x65 -l %s", SI_CTL, direction == SCAN_FORWARD ? "up" : "down");
	fp = popen(cmd, "r");
	if(fp == NULL) {
		AFB_ERROR("Could not run: %s!", cmd);
		return;
	}
	// Look for "Frequency:" in output
	while(fgets(line, 128, fp) != NULL) {
		if(strncmp("Frequency:", line, 10) == 0) {
			new_frequency = atoi(line + 10);
			//AFB_DEBUG("%s: got new_frequency = %d", __FUNCTION__, new_frequency);
			break;
		}
	}

	// Make sure si_ctl has finished
	rc = pclose(fp);
	if(rc != 0) {
		// Make sure we reset to original frequency, the Si4689 seems
		// to auto-mute sometimes on failed scans, this hopefully works
		// around that.
		new_frequency = 0;
	}

	if(new_frequency) {
		current_frequency = new_frequency * 1000;

		// Push up the new frequency
		// This is more efficient than calling kf_set_frequency and calling
		// out to si_ctl again.
		if(freq_callback)
			freq_callback(current_frequency, freq_callback_data);
	} else {
		// Assume no station found, go back to starting frequency
		kf_set_frequency(current_frequency);
	}

	// Push up scan state
	if(callback)
		callback(current_frequency, data);

	scanning = false;
}

static void kf_scan_stop(void)
{
	// ATM, it's not straightforward to stop a scan since we're using the si_ctl utility...
}

static radio_stereo_mode_t kf_get_stereo_mode(void)
{
	return STEREO;
}

static void kf_set_stereo_mode(radio_stereo_mode_t mode)
{
	// We only support stereo, so do nothing
}

radio_impl_ops_t kf_impl_ops = {
	.name = "Kingfisher Si4689",
	.init = kf_init,
	.get_frequency = kf_get_frequency,
	.set_frequency = kf_set_frequency,
	.set_frequency_callback = kf_set_frequency_callback,
	.get_band = kf_get_band,
	.set_band = kf_set_band,
	.band_supported = kf_band_supported,
	.get_min_frequency = kf_get_min_frequency,
	.get_max_frequency = kf_get_max_frequency,
	.get_frequency_step = kf_get_frequency_step,
	.start = kf_start,
	.stop = kf_stop,
	.scan_start = kf_scan_start,
	.scan_stop = kf_scan_stop,
	.get_stereo_mode = kf_get_stereo_mode,
	.set_stereo_mode = kf_set_stereo_mode
};
