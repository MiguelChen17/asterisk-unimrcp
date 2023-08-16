/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Molo Afrika Speech Technologies (Pty) Ltd
 *
 * J.W.F. Thirion <derik@molo.co.za>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * Please follow coding guidelines 
 * http://svn.digium.com/view/asterisk/trunk/doc/CODING-GUIDELINES
 */

/* By Molo Afrika Speech Technologies (Pty) Ltd
 *     See: http://www.molo.co.za
 *
 * Ideas, concepts and code borrowed from UniMRCP's example programs
 * and the FreeSWITCH mod_unimrcp ASR/TTS module.
 *
 * Authors of these are:
 *     UniMRCP:
 *         Arsen Chaloyan <achaloyan@gmail.com>
 *     FreeSWITCH: mod_unimrcp
 *         Christopher M. Rienzo <chris@rienzo.net>
 *
 * See:
 *     http://www.unimrcp.org
 *     http://www.freeswitch.org
 */

/*! \file
 *
 * \brief MRCPSynth application
 *
 * \author\verbatim J.W.F. Thirion <derik@molo.co.za> \endverbatim
 * 
 * MRCPSynth application
 * \ingroup applications
 */

/* Asterisk includes. */
#include "ast_compat_defs.h"

#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/app.h"

/* UniMRCP includes. */
#include "app_datastore.h"
#include "app_msg_process_dispatcher.h"

/*** DOCUMENTATION
	<application name="MRCPSynth" language="en_US">
		<synopsis>
			MRCP synthesis application.
		</synopsis>
		<syntax>
			<parameter name="prompt" required="true">
				<para>A prompt specified as a plain text, an SSML content, or by means of a file or URI reference.</para>
			</parameter>
			<parameter name="options" required="false">
				<optionlist>
					<option name="p"> <para>Profile to use in mrcp.conf.</para> </option>
					<option name="i"> <para>Digits to allow the TTS to be interrupted with.</para> </option>
					<option name="f"> <para>Filename on disk to store audio to (audio not stored if not specified or empty).</para> </option>
					<option name="l"> <para>Language to use (e.g. "en-GB", "en-US", "en-AU", etc.).</para> </option>
					<option name="ll"> <para>Load lexicon (true/false).</para> </option>
					<option name="pv"> <para>Prosody volume (silent/x-soft/soft/medium/loud/x-loud/default).</para> </option>
					<option name="pr"> <para>Prosody rate (x-slow/slow/medium/fast/x-fast/default).</para> </option>
					<option name="v"> <para>Voice name to use (e.g. "Daniel", "Karin", etc.).</para> </option>
					<option name="g"> <para>Voice gender to use (e.g. "male", "female").</para> </option>
					<option name="vv"> <para>Voice variant.</para> </option>
					<option name="a"> <para>Voice age.</para> </option>
					<option name="plt"> <para>Persistent lifetime (0: no [MRCP session is created and destroyed dynamically],
						1: yes [MRCP session is created on demand, reused and destroyed on hang-up].</para>
					</option>
					<option name="dse"> <para>Datastore entry.</para></option>
					<option name="sbs"> <para>Always stop barged synthesis request.</para></option>
					<option name="vsp"> <para>Vendor-specific parameters.</para></option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>This application establishes an MRCP session for speech synthesis.</para>
			<para>If synthesis completed, the variable ${SYNTHSTATUS} is set to "OK"; otherwise, if an error occurred, 
			the variable ${SYNTHSTATUS} is set to "ERROR". If the caller hung up while the synthesis was in-progress, 
			the variable ${SYNTHSTATUS} is set to "INTERRUPTED".</para>
			<para>The variable ${SYNTH_COMPLETION_CAUSE} indicates whether synthesis completed normally or with an error.
			("000" - normal, "001" - barge-in, "002" - parse-failure, ...) </para>
		</description>
		<see-also>
			<ref type="application">MRCPRecog</ref>
			<ref type="application">SynthAndRecog</ref>
		</see-also>
	</application>
 ***/

/* The name of the application. */
static const char *app_synth = "MRCPSynth";

/* The application instance. */
static ast_mrcp_application_t *mrcpsynth = NULL;

/* The enumeration of application options (excluding the MRCP params). */
enum mrcpsynth_option_flags {
	MRCPSYNTH_PROFILE             = (1 << 0),
	MRCPSYNTH_INTERRUPT           = (1 << 1),
	MRCPSYNTH_FILENAME            = (1 << 2),
	MRCPSYNTH_PERSISTENT_LIFETIME = (1 << 3),
	MRCPSYNTH_DATASTORE_ENTRY     = (1 << 4),
	MRCPSYNTH_STOP_BARGED_SYNTH   = (1 << 5)
};

/* The enumeration of option arguments. */
enum mrcpsynth_option_args {
	OPT_ARG_PROFILE             = 0,
	OPT_ARG_INTERRUPT           = 1,
	OPT_ARG_FILENAME            = 2,
	OPT_ARG_PERSISTENT_LIFETIME = 3,
	OPT_ARG_DATASTORE_ENTRY     = 4,
	OPT_ARG_STOP_BARGED_SYNTH   = 5,

	/* This MUST be the last value in this enum! */
	OPT_ARG_ARRAY_SIZE = 6
};

/* The structure which holds the application options (including the MRCP params). */
struct mrcpsynth_options_t {
	apr_hash_t *synth_hfs;
	apr_hash_t *vendor_par_list;

	int         flags;
	const char *params[OPT_ARG_ARRAY_SIZE];
};

typedef struct mrcpsynth_options_t mrcpsynth_options_t;

/* --- MRCP SPEECH CHANNEL INTERFACE TO UNIMRCP --- */

/* Get speech channel associated with provided MRCP session. */
static APR_INLINE speech_channel_t * get_speech_channel(mrcp_session_t *session)
{
	if (session)
		return (speech_channel_t *)mrcp_application_session_object_get(session);

	return NULL;
}

/* --- MRCP TTS --- */

/* Process UniMRCP messages for the synthesizer application.  All MRCP synthesizer callbacks start here first. */
static apt_bool_t synth_message_handler(const mrcp_app_message_t *app_message)
{
	/* Call the appropriate callback in the dispatcher function table based on the app_message received. */
	if (app_message)
		return mrcp_application_message_dispatch(&mrcpsynth->dispatcher, app_message);

	ast_log(LOG_ERROR, "(unknown) app_message error!\n");
	return TRUE;
}

/* Incoming TTS data from UniMRCP. */
static apt_bool_t synth_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
	speech_channel_t *schannel;

	if (stream)
		schannel = (speech_channel_t *)stream->obj;
	else
		schannel = NULL;

	if(!schannel || !frame) {
		ast_log(LOG_ERROR, "synth_stream_write: unknown channel error!\n");
		return FALSE;
	}

	if (frame->codec_frame.size > 0 && (frame->type & MEDIA_FRAME_TYPE_AUDIO) == MEDIA_FRAME_TYPE_AUDIO) {
		speech_channel_ast_write(schannel, frame->codec_frame.buffer, frame->codec_frame.size);
	}

	return TRUE;
}

/* Send SPEAK request to synthesizer. */
static int synth_channel_speak(speech_channel_t *schannel, const char *content, const char *content_type, mrcpsynth_options_t *options)
{
	int status = 0;
	mrcp_message_t *mrcp_message = NULL;
	mrcp_generic_header_t *generic_header = NULL;
	mrcp_synth_header_t *synth_header = NULL;

	if (!schannel || !content || !content_type) {
		ast_log(LOG_ERROR, "synth_channel_speak: unknown channel error!\n");
		return -1;
	}

	apr_thread_mutex_lock(schannel->mutex);

	if (schannel->state != SPEECH_CHANNEL_READY) {
		apr_thread_mutex_unlock(schannel->mutex);
		return -1;
	}

	if ((mrcp_message = mrcp_application_message_create(schannel->session->unimrcp_session,
														schannel->unimrcp_channel,
														SYNTHESIZER_SPEAK)) == NULL) {
		ast_log(LOG_ERROR, "(%s) Failed to create SPEAK message\n", schannel->name);

		apr_thread_mutex_unlock(schannel->mutex);
		return -1;
	}

	/* Set generic header fields (content-type). */
	if ((generic_header = (mrcp_generic_header_t *)mrcp_generic_header_prepare(mrcp_message)) == NULL) {	
		apr_thread_mutex_unlock(schannel->mutex);
		return -1;
	}

	apt_string_assign(&generic_header->content_type, content_type, mrcp_message->pool);
	mrcp_generic_header_property_add(mrcp_message, GENERIC_HEADER_CONTENT_TYPE);

	/* Set synthesizer header fields (voice, rate, etc.). */
	if ((synth_header = (mrcp_synth_header_t *)mrcp_resource_header_prepare(mrcp_message)) == NULL) {
		apr_thread_mutex_unlock(schannel->mutex);
		return -1;
	}

	/* Add params to MRCP message. */
	speech_channel_set_params(schannel, mrcp_message, options->synth_hfs, options->vendor_par_list);

	/* Set body (plain text or SSML). */
	apt_string_assign(&mrcp_message->body, content, schannel->pool);

	/* Empty audio queue and send SPEAK to MRCP server. */
	audio_queue_clear(schannel->audio_queue);

	if (!mrcp_application_message_send(schannel->session->unimrcp_session, schannel->unimrcp_channel, mrcp_message)) {
		ast_log(LOG_ERROR,"(%s) Failed to send SPEAK message", schannel->name);

		apr_thread_mutex_unlock(schannel->mutex);
		return -1;
	}

	/* Wait for IN PROGRESS. */
	apr_thread_cond_timedwait(schannel->cond, schannel->mutex, globals.speech_channel_timeout);

	if (schannel->state != SPEECH_CHANNEL_PROCESSING) {
		apr_thread_mutex_unlock(schannel->mutex);
		return -1;
	}

	apr_thread_mutex_unlock(schannel->mutex);
	return status;
}

/* Apply application options. */
static int mrcpsynth_option_apply(mrcpsynth_options_t *options, const char *key, char *value)
{
	char *vendor_name, *vendor_value;
	if (strcasecmp(key, "p") == 0) {
		options->flags |= MRCPSYNTH_PROFILE;
		options->params[OPT_ARG_PROFILE] = value;
	} else if (strcasecmp(key, "i") == 0) {
		options->flags |= MRCPSYNTH_INTERRUPT;
		options->params[OPT_ARG_INTERRUPT] = value;
	} else if (strcasecmp(key, "f") == 0) {
		options->flags |= MRCPSYNTH_FILENAME;
		options->params[OPT_ARG_FILENAME] = value;
	} else if (strcasecmp(key, "l") == 0) {
		apr_hash_set(options->synth_hfs, "Speech-Language", APR_HASH_KEY_STRING, value);
	} else if (strcasecmp(key, "ll") == 0) {
		apr_hash_set(options->synth_hfs, "Load-Lexicon", APR_HASH_KEY_STRING, value);
	} else if (strcasecmp(key, "pv") == 0) {
		apr_hash_set(options->synth_hfs, "Prosody-Volume", APR_HASH_KEY_STRING, value);
	} else if (strcasecmp(key, "pr") == 0) {
		apr_hash_set(options->synth_hfs, "Prosody-Rate", APR_HASH_KEY_STRING, value);
	} else if (strcasecmp(key, "v") == 0) {
		apr_hash_set(options->synth_hfs, "Voice-Name", APR_HASH_KEY_STRING, value);
	} else if (strcasecmp(key, "vv") == 0) {
		apr_hash_set(options->synth_hfs, "Voice-Variant", APR_HASH_KEY_STRING, value);
	} else if (strcasecmp(key, "g") == 0) {
		apr_hash_set(options->synth_hfs, "Voice-Gender", APR_HASH_KEY_STRING, value);
	} else if (strcasecmp(key, "a") == 0) {
		apr_hash_set(options->synth_hfs, "Voice-Age", APR_HASH_KEY_STRING, value);
	} else if (strcasecmp(key, "vsp") == 0) {
		vendor_value = value;
		if ((vendor_name = strsep(&vendor_value, "=")) && vendor_value) {
			apr_hash_set(options->vendor_par_list, vendor_name, APR_HASH_KEY_STRING, vendor_value);
		}
	} else if (strcasecmp(key, "plt") == 0) {
		options->flags |= MRCPSYNTH_PERSISTENT_LIFETIME;
		options->params[OPT_ARG_PERSISTENT_LIFETIME] = value;
	} else if (strcasecmp(key, "dse") == 0) {
		options->flags |= MRCPSYNTH_DATASTORE_ENTRY;
		options->params[OPT_ARG_DATASTORE_ENTRY] = value;
	} else if (strcasecmp(key, "sbs") == 0) {
		options->flags |= MRCPSYNTH_STOP_BARGED_SYNTH;
		options->params[OPT_ARG_STOP_BARGED_SYNTH] = value;
	} else {
		ast_log(LOG_WARNING, "Unknown option: %s\n", key);
	}
	return 0;
}

/* Parse application options. */
static int mrcpsynth_options_parse(char *str, mrcpsynth_options_t *options, apr_pool_t *pool)
{
	char *s;
	char *name, *value;
	
	if (!str)
		return 0;

	if ((options->synth_hfs = apr_hash_make(pool)) == NULL) {
		return -1;
	}

	if ((options->vendor_par_list = apr_hash_make(pool)) == NULL) {
		return -1;
	}

	while ((s = strsep(&str, "&"))) {
		value = s;
		if ((name = strsep(&value, "=")) && value) {
			ast_log(LOG_DEBUG, "Apply option %s: %s\n", name, value);
			mrcpsynth_option_apply(options, name, value);
		}
	}
	return 0;
}

/* Exit the application. */
static int mrcpsynth_exit(struct ast_channel *chan, app_session_t *app_session, speech_channel_status_t status)
{
	if (app_session) {
		if (app_session->writeformat && app_session->rawwriteformat)
			ast_set_write_format_path(chan, app_session->writeformat, app_session->rawwriteformat);

		if (app_session->synth_channel) {
			if (app_session->lifetime == APP_SESSION_LIFETIME_DYNAMIC) {
				if (app_session->stop_barged_synth == TRUE) {
					speech_channel_stop(app_session->synth_channel);
				}
				speech_channel_destroy(app_session->synth_channel);
				app_session->synth_channel = NULL;
			}
		}
	}

	const char *status_str = speech_channel_status_to_string(status);
	pbx_builtin_setvar_helper(chan, "SYNTHSTATUS", status_str);
	ast_log(LOG_NOTICE, "%s() exiting status: %s on %s\n", app_synth, status_str, ast_channel_name(chan));
	return 0;
}

/* The entry point of the application. */
static int app_synth_exec(struct ast_channel *chan, ast_app_data data)
{
	struct ast_frame *f;
	apr_uint32_t speech_channel_number = get_next_speech_channel_number();
	const char *name;
	speech_channel_status_t status;
	char *parse;
	int i;
	mrcpsynth_options_t mrcpsynth_options;

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(prompt);
		AST_APP_ARG(options);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "%s() requires an argument (prompt[,options])\n", app_synth);
		return mrcpsynth_exit(chan, NULL, SPEECH_CHANNEL_STATUS_ERROR);
	}

	/* We need to make a copy of the input string if we are going to modify it! */
	parse = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, parse);

	if (ast_strlen_zero(args.prompt)) {
		ast_log(LOG_WARNING, "%s() requires a prompt argument (prompt[,options])\n", app_synth);
		return mrcpsynth_exit(chan, NULL, SPEECH_CHANNEL_STATUS_ERROR);
	}

	args.prompt = normalize_input_string(args.prompt);
	ast_log(LOG_NOTICE, "%s() prompt: %s\n", app_synth, args.prompt);

	app_datastore_t* datastore = app_datastore_get(chan);
	if (!datastore) {
		ast_log(LOG_ERROR, "Unable to retrieve data from app datastore on %s\n", ast_channel_name(chan));
		return mrcpsynth_exit(chan, NULL, SPEECH_CHANNEL_STATUS_ERROR);
	}

	mrcpsynth_options.synth_hfs = NULL;
	mrcpsynth_options.flags = 0;
	for (i=0; i<OPT_ARG_ARRAY_SIZE; i++)
		mrcpsynth_options.params[i] = NULL;

	if (!ast_strlen_zero(args.options)) {
		args.options = normalize_input_string(args.options);
		ast_log(LOG_NOTICE, "%s() options: %s\n", app_synth, args.options);
		char *options_buf = apr_pstrdup(datastore->pool, args.options);
		mrcpsynth_options_parse(options_buf, &mrcpsynth_options, datastore->pool);
	}

	int dtmf_enable = 0;
	if ((mrcpsynth_options.flags & MRCPSYNTH_INTERRUPT) == MRCPSYNTH_INTERRUPT) {
		if (!ast_strlen_zero(mrcpsynth_options.params[OPT_ARG_INTERRUPT])) {
			dtmf_enable = 1;

			if (strcasecmp(mrcpsynth_options.params[OPT_ARG_INTERRUPT], "any") == 0) {
				mrcpsynth_options.params[OPT_ARG_INTERRUPT] = AST_DIGIT_ANY;
			} else if (strcasecmp(mrcpsynth_options.params[OPT_ARG_INTERRUPT], "none") == 0)
				dtmf_enable = 0;
		}
	}

	/* Answer if it's not already going. */
	if (ast_channel_state(chan) != AST_STATE_UP)
		ast_answer(chan);

	/* Ensure no streams are currently playing. */
	ast_stopstream(chan);

	/* Set default lifetime to dynamic. */
	int lifetime = APP_SESSION_LIFETIME_DYNAMIC;

	/* Get datastore entry. */
	const char *entry = ast_channel_name(chan);
	if ((mrcpsynth_options.flags & MRCPSYNTH_DATASTORE_ENTRY) == MRCPSYNTH_DATASTORE_ENTRY) {
		if (!ast_strlen_zero(mrcpsynth_options.params[OPT_ARG_DATASTORE_ENTRY])) {
			entry = mrcpsynth_options.params[OPT_ARG_DATASTORE_ENTRY];
			lifetime = APP_SESSION_LIFETIME_PERSISTENT;
		}
	}

	/* Check session lifetime. */
	if ((mrcpsynth_options.flags & MRCPSYNTH_PERSISTENT_LIFETIME) == MRCPSYNTH_PERSISTENT_LIFETIME) {
		if (!ast_strlen_zero(mrcpsynth_options.params[OPT_ARG_PERSISTENT_LIFETIME])) {
			lifetime = (atoi(mrcpsynth_options.params[OPT_ARG_PERSISTENT_LIFETIME]) == 0) ? 
				APP_SESSION_LIFETIME_DYNAMIC : APP_SESSION_LIFETIME_PERSISTENT;
		}
	}
	
	/* Get application datastore. */
	ast_log(LOG_NOTICE, "%s() Using datastore entry: %s\n", app_synth, entry);
	app_session_t *app_session = app_datastore_session_add(datastore, entry);
	if (!app_session) {
		return mrcpsynth_exit(chan, NULL, SPEECH_CHANNEL_STATUS_ERROR);
	}
	app_session->lifetime = lifetime;
	app_session->msg_process_dispatcher = &mrcpsynth->message_process;

	/* Check whether or not to always stop barged synthesis request. */
	app_session->stop_barged_synth = FALSE;
	if ((mrcpsynth_options.flags & MRCPSYNTH_STOP_BARGED_SYNTH) == MRCPSYNTH_STOP_BARGED_SYNTH) {
		if (!ast_strlen_zero(mrcpsynth_options.params[OPT_ARG_STOP_BARGED_SYNTH])) {
			app_session->stop_barged_synth = (atoi(mrcpsynth_options.params[OPT_ARG_STOP_BARGED_SYNTH]) == 0) ? FALSE : TRUE;
		}
	}

	if(!app_session->synth_channel) {
		const char *filename = NULL;
		if ((mrcpsynth_options.flags & MRCPSYNTH_FILENAME) == MRCPSYNTH_FILENAME) {
			filename = mrcpsynth_options.params[OPT_ARG_FILENAME];
		}

		/* Get new write format. */
		app_session->nwriteformat = ast_channel_get_speechwriteformat(chan, app_session->pool);

		name = apr_psprintf(app_session->pool, "TTS-%lu", (unsigned long int)speech_channel_number);

		/* Create speech channel for synthesis. */
		app_session->synth_channel = speech_channel_create(
										app_session->pool,
										name,
										SPEECH_CHANNEL_SYNTHESIZER,
										mrcpsynth,
										app_session->nwriteformat,
										filename,
										chan,
										NULL);
		if (!app_session->synth_channel) {
			return mrcpsynth_exit(chan, app_session, SPEECH_CHANNEL_STATUS_ERROR);
		}
		app_session->synth_channel->app_session = app_session;

		const char *profile_name = NULL;
		if ((mrcpsynth_options.flags & MRCPSYNTH_PROFILE) == MRCPSYNTH_PROFILE) {
			if (!ast_strlen_zero(mrcpsynth_options.params[OPT_ARG_PROFILE])) {
				profile_name = mrcpsynth_options.params[OPT_ARG_PROFILE];
			}
		}

		/* Get synthesis profile. */
		ast_mrcp_profile_t *profile = get_synth_profile(profile_name);
		if (!profile) {
			ast_log(LOG_ERROR, "(%s) Can't find profile, %s\n", name, profile_name);
			return mrcpsynth_exit(chan, app_session, SPEECH_CHANNEL_STATUS_ERROR);
		}

		/* Open synthesis channel. */
		if (speech_channel_open(app_session->synth_channel, profile) != 0) {
			return mrcpsynth_exit(chan, app_session, SPEECH_CHANNEL_STATUS_ERROR);
		}
	}
	else {
		name = app_session->synth_channel->name;
	}
	
	/* Get old write format. */
	ast_format_compat *owriteformat = ast_channel_get_writeformat(chan, app_session->pool);
	ast_format_compat *orawwriteformat = ast_channel_get_rawwriteformat(chan, app_session->pool);

	/* Set write format. */
	ast_set_write_format_path(chan, app_session->nwriteformat, orawwriteformat);

	/* Store old write format. */
	app_session->writeformat = owriteformat;
	app_session->rawwriteformat = orawwriteformat;

	const char *content = NULL;
	const char *content_type = NULL;
	if (determine_synth_content_type(app_session->synth_channel, args.prompt, &content, &content_type) != 0) {
		ast_log(LOG_WARNING, "(%s) Unable to determine synthesis content type\n", name);
		return mrcpsynth_exit(chan, app_session, SPEECH_CHANNEL_STATUS_ERROR);
	}

	ast_log(LOG_NOTICE, "(%s) Synthesizing, enable DTMFs: %d\n", name, dtmf_enable);

	if (synth_channel_speak(app_session->synth_channel, content, content_type, &mrcpsynth_options) != 0) {
		ast_log(LOG_WARNING, "(%s) Unable to start synthesis\n", name);
		return mrcpsynth_exit(chan, app_session, SPEECH_CHANNEL_STATUS_ERROR);
	}

	int ms;
	int running;
	status = SPEECH_CHANNEL_STATUS_OK;
	do {
		ms = ast_waitfor(chan, 100);
		if (ms < 0) {
			ast_log(LOG_DEBUG, "(%s) Hangup detected\n", name);
			return mrcpsynth_exit(chan, app_session, SPEECH_CHANNEL_STATUS_INTERRUPTED);
		}

		f = ast_read(chan);
		if (!f) {
			ast_log(LOG_DEBUG, "(%s) Null frame == hangup() detected\n", name);
			return mrcpsynth_exit(chan, app_session, SPEECH_CHANNEL_STATUS_INTERRUPTED);
		}

		running = 1;
		if (dtmf_enable && f->frametype == AST_FRAME_DTMF) {
			int dtmfkey = ast_frame_get_dtmfkey(f);

			ast_log(LOG_DEBUG, "(%s) User pressed a key (%d)\n", name, dtmfkey);
			if (mrcpsynth_options.params[OPT_ARG_INTERRUPT] && strchr(mrcpsynth_options.params[OPT_ARG_INTERRUPT], dtmfkey)) {
				status = SPEECH_CHANNEL_STATUS_INTERRUPTED;
				running = 0;

				ast_log(LOG_DEBUG, "(%s) Sending BARGE-IN-OCCURRED\n", app_session->synth_channel->name);
				if (speech_channel_bargeinoccurred(app_session->synth_channel) != 0) {
					ast_log(LOG_ERROR, "(%s) Failed to send BARGE-IN-OCCURRED\n", app_session->synth_channel->name);
				}
			}
		}

		ast_frfree(f);

		if (app_session->synth_channel->state != SPEECH_CHANNEL_PROCESSING) {
			/* end of prompt */
			running = 0;
		}
	}
	while (running);

	return mrcpsynth_exit(chan, app_session, status);
}

int load_mrcpsynth_app()
{
	apr_pool_t *pool = globals.pool;

	if (pool == NULL) {
		ast_log(LOG_ERROR, "Memory pool is NULL\n");
		return -1;
	}

	if(mrcpsynth) {
		ast_log(LOG_ERROR, "Application %s is already loaded\n", app_synth);
		return -1;
	}

	mrcpsynth = (ast_mrcp_application_t*) apr_palloc(pool, sizeof(ast_mrcp_application_t));
	mrcpsynth->name = app_synth;
	mrcpsynth->exec = app_synth_exec;
#if !AST_VERSION_AT_LEAST(1,6,2)
	mrcpsynth->synopsis = NULL;
	mrcpsynth->description = NULL;
#endif

	/* Create the synthesizer application and link its callbacks */
	if ((mrcpsynth->app = mrcp_application_create(synth_message_handler, (void *)mrcpsynth, pool)) == NULL) {
		ast_log(LOG_ERROR, "Unable to create synthesizer MRCP application %s\n", app_synth);
		mrcpsynth = NULL;
		return -1;
	}

	mrcpsynth->dispatcher.on_session_update = NULL;
	mrcpsynth->dispatcher.on_session_terminate = speech_on_session_terminate;
	mrcpsynth->dispatcher.on_channel_add = speech_on_channel_add;
	mrcpsynth->dispatcher.on_channel_remove = NULL;
	mrcpsynth->dispatcher.on_message_receive = mrcp_on_message_receive;
	mrcpsynth->message_process.synth_message_process = synth_on_message_receive;
	mrcpsynth->message_process.verif_message_process = verif_on_message_receive;
	mrcpsynth->message_process.recog_message_process = recog_on_message_receive;
	mrcpsynth->dispatcher.on_terminate_event = NULL;
	mrcpsynth->dispatcher.on_resource_discover = NULL;
	mrcpsynth->audio_stream_vtable.destroy = NULL;
	mrcpsynth->audio_stream_vtable.open_rx = NULL;
	mrcpsynth->audio_stream_vtable.close_rx = NULL;
	mrcpsynth->audio_stream_vtable.read_frame = NULL;
	mrcpsynth->audio_stream_vtable.open_tx = NULL;
	mrcpsynth->audio_stream_vtable.close_tx =  NULL;
	mrcpsynth->audio_stream_vtable.write_frame = synth_stream_write;
	mrcpsynth->audio_stream_vtable.trace = NULL;

	if (!mrcp_client_application_register(globals.mrcp_client, mrcpsynth->app, app_synth)) {
		ast_log(LOG_ERROR, "Unable to register synthesizer MRCP application %s\n", app_synth);
		if (!mrcp_application_destroy(mrcpsynth->app))
			ast_log(LOG_WARNING, "Unable to destroy synthesizer MRCP application %s\n", app_synth);
		mrcpsynth = NULL;
		return -1;
	}

	apr_hash_set(globals.apps, app_synth, APR_HASH_KEY_STRING, mrcpsynth);

	return 0;
}

int unload_mrcpsynth_app()
{
	if(!mrcpsynth) {
		ast_log(LOG_ERROR, "Application %s doesn't exist\n", app_synth);
		return -1;
	}

	apr_hash_set(globals.apps, app_synth, APR_HASH_KEY_STRING, NULL);
	mrcpsynth = NULL;

	return 0;
}
