/*!
 * \file        ast115.c
 * \brief       SCCP PBX Asterisk Wrapper Class
 * \author      Marcello Ceshia
 * \author      Diederik de Groot <ddegroot [at] users.sourceforge.net>
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 */

#include "config.h"
#include "common.h"
#include "chan_sccp.h"
#include "sccp_pbx.h"
#include "sccp_channel.h"
#include "sccp_device.h"
#include "sccp_line.h"
#include "sccp_cli.h"
#include "sccp_utils.h"
#include "sccp_indicate.h"
#include "sccp_hint.h"
#include "sccp_mwi.h"
#include "sccp_appfunctions.h"
#include "sccp_management.h"
#include "sccp_netsock.h"
#include "sccp_rtp.h"
#include "sccp_session.h"		// required for sccp_session_getOurIP
#include "sccp_labels.h"
#include "ast115.h"
#include "ast_announce.h"

SCCP_FILE_VERSION(__FILE__, "");

__BEGIN_C_EXTERN__
#ifdef HAVE_PBX_ACL_H
#  include <asterisk/acl.h>
#endif
#include <asterisk/module.h>
#include <asterisk/callerid.h>
#include <asterisk/causes.h>
#include <asterisk/musiconhold.h>
#ifdef HAVE_PBX_FEATURES_H
#  include <asterisk/features_config.h>
#  include <asterisk/features.h>
#  include <asterisk/parking.h>
#  include <asterisk/pickup.h>
#endif
#ifdef HAVE_PBX_APP_H
#  include <asterisk/app.h>
#endif
#include <asterisk/translate.h>
#include <asterisk/indications.h>
#include <asterisk/cel.h>
#include <asterisk/netsock2.h>
#include <asterisk/pickup.h>
#include <asterisk/stasis.h>
#include <asterisk/stasis_channels.h>
#include <asterisk/stasis_endpoints.h>
#include <asterisk/bridge_after.h>
#include <asterisk/bridge_channel.h>

#define new avoid_cxx_new_keyword
#include <asterisk/rtp_engine.h>
#undef new
#include <asterisk/timing.h>
__END_C_EXTERN__

struct ast_sched_context *sched = 0;
struct io_context *io = 0;

//struct ast_format slinFormat = { AST_FORMAT_SLINEAR, {{0}, 0} };

static PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_request(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const PBX_CHANNEL_TYPE * requestor, const char *dest, int *cause);
static int sccp_wrapper_asterisk115_call(PBX_CHANNEL_TYPE * ast, const char *dest, int timeout);
static int sccp_wrapper_asterisk115_answer(PBX_CHANNEL_TYPE * chan);
static PBX_FRAME_TYPE *sccp_wrapper_asterisk115_rtp_read(PBX_CHANNEL_TYPE * ast);
static int sccp_wrapper_asterisk115_rtp_write(PBX_CHANNEL_TYPE * ast, PBX_FRAME_TYPE * frame);
static int sccp_wrapper_asterisk115_indicate(PBX_CHANNEL_TYPE * ast, int ind, const void *data, size_t datalen);
static int sccp_wrapper_asterisk115_fixup(PBX_CHANNEL_TYPE * oldchan, PBX_CHANNEL_TYPE * newchan);
static void sccp_wrapper_asterisk115_setDialedNumber(constChannelPtr channel, const char *number);

//#ifdef CS_AST_RTP_INSTANCE_BRIDGE
//static enum ast_bridge_result sccp_wrapper_asterisk115_rtpBridge(PBX_CHANNEL_TYPE * c0, PBX_CHANNEL_TYPE * c1, int flags, PBX_FRAME_TYPE ** fo, PBX_CHANNEL_TYPE ** rc, int timeoutms);
//#endif
static int sccp_pbx_sendtext(PBX_CHANNEL_TYPE * ast, const char *text);
static int sccp_wrapper_recvdigit_begin(PBX_CHANNEL_TYPE * ast, char digit);
static int sccp_wrapper_recvdigit_end(PBX_CHANNEL_TYPE * ast, char digit, unsigned int duration);
static int sccp_pbx_sendHTML(PBX_CHANNEL_TYPE * ast, int subclass, const char *data, int datalen);
int sccp_wrapper_asterisk115_hangup(PBX_CHANNEL_TYPE * ast_channel);
int sccp_asterisk_queue_control(const PBX_CHANNEL_TYPE * pbx_channel, enum ast_control_frame_type control);
int sccp_asterisk_queue_control_data(const PBX_CHANNEL_TYPE * pbx_channel, enum ast_control_frame_type control, const void *data, size_t datalen);
static int sccp_wrapper_asterisk115_devicestate(const char *data);
static boolean_t sccp_wrapper_asterisk115_setWriteFormat(constChannelPtr channel, skinny_codec_t codec);
static boolean_t sccp_wrapper_asterisk115_setReadFormat(constChannelPtr channel, skinny_codec_t codec);
PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_findPickupChannelByExtenLocked(PBX_CHANNEL_TYPE * chan, const char *exten, const char *context);
PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_findPickupChannelByGroupLocked(PBX_CHANNEL_TYPE * chan);

static inline skinny_codec_t sccp_asterisk115_getSkinnyFormatSingle(struct ast_format_cap *ast_format_capability)
{
	uint formatPosition;
	skinny_codec_t codec = SKINNY_CODEC_NONE;
	struct ast_format *format;
	
	for (formatPosition = 0; formatPosition < ast_format_cap_count(ast_format_capability); ++formatPosition) {
		format = ast_format_cap_get_format(ast_format_capability, formatPosition);
		uint64_t ast_codec = ast_format_compatibility_format2bitfield(format);
		ao2_ref(format, -1);

		if ((codec = pbx_codec2skinny_codec(ast_codec))== SKINNY_CODEC_NONE) {
			ast_log(LOG_WARNING, "SCCP: (getSkinnyFormatSingle) No matching codec found");
			break;
		}
	}

	return codec;
}

static uint8_t sccp_asterisk115_getSkinnyFormatMultiple(struct ast_format_cap *ast_format_capability, skinny_codec_t codec[], int length)
{
	// struct ast_format tmp_fmt;
	uint formatPosition;
	skinny_codec_t found = SKINNY_CODEC_NONE;
	uint8_t position = 0;
	struct ast_str *codec_buf = ast_str_alloca(64);
	struct ast_format *format;

	sccp_log(DEBUGCAT_RTP)(VERBOSE_PREFIX_3 "SCCP: (getSkinnyFormatSingle) caps %s\n", ast_format_cap_get_names(ast_format_capability,&codec_buf));

	for (formatPosition = 0; formatPosition < ast_format_cap_count(ast_format_capability); ++formatPosition) {
		format = ast_format_cap_get_format(ast_format_capability, formatPosition);
		uint64_t ast_codec = ast_format_compatibility_format2bitfield(format);
		ao2_ref(format, -1);

		if ((found = pbx_codec2skinny_codec(ast_codec)) != SKINNY_CODEC_NONE) {
			codec[position++] = found;
			break;
		}
	}

	return position;
}

static struct ast_format *sccp_asterisk115_skinny2ast_format(skinny_codec_t skinnycodec)
{
	switch (skinnycodec) {
		case SKINNY_CODEC_G711_ALAW_64K:
		case SKINNY_CODEC_G711_ALAW_56K:
			return ast_format_alaw;
		case SKINNY_CODEC_G711_ULAW_64K:
		case SKINNY_CODEC_G711_ULAW_56K:
			return ast_format_ulaw;
		case SKINNY_CODEC_G722_64K:
		case SKINNY_CODEC_G722_56K:
		case SKINNY_CODEC_G722_48K:
			return ast_format_g722;
		case SKINNY_CODEC_G723_1:
			return ast_format_g723;
		case SKINNY_CODEC_G729:
		case SKINNY_CODEC_G729_A:
			return ast_format_g729;
		case SKINNY_CODEC_G726_32K:
			return ast_format_g726;
		case SKINNY_CODEC_H261:
			return ast_format_h261;
		case SKINNY_CODEC_H263:
			return ast_format_h263;
		case SKINNY_CODEC_H264:
			return ast_format_h264;
		default:
			return ast_format_none;
	}

}

static void pbx_format_cap_append_skinny(struct ast_format_cap *caps, skinny_codec_t codecs[SKINNY_MAX_CAPABILITIES]) {
	int i;
	for (i=0; i<SKINNY_MAX_CAPABILITIES; i++) {
		if (codecs[i] == SKINNY_CODEC_NONE) {
			break;
		}
		struct ast_format *format = sccp_asterisk115_skinny2ast_format(codecs[i]);
		if (format != ast_format_none) {
			unsigned int framing = ast_format_get_default_ms(format);
			ast_format_cap_append(caps, format, framing);
		}
	}
}

#if defined(__cplusplus) || defined(c_plusplus)
/*!
 * \brief NULL Tech Structure
 */
static const struct ast_channel_tech null_tech = {
type:	"NULL",
description:"Null channel (should not see this)",
};

/*!
 * \brief SCCP Tech Structure
 */
static struct ast_channel_tech sccp_tech = {
	/* *INDENT-OFF* */
	type:			SCCP_TECHTYPE_STR,
	description:		"Skinny Client Control Protocol (SCCP)",
//      capabilities:		AST_FORMAT_ALAW | AST_FORMAT_ULAW | AST_FORMAT_SLINEAR16 | AST_FORMAT_GSM | AST_FORMAT_G723_1 | AST_FORMAT_G729A | AST_FORMAT_H264 | AST_FORMAT_H263_PLUS,
	properties:		AST_CHAN_TP_WANTSJITTER | AST_CHAN_TP_CREATESJITTER,
	requester:		sccp_wrapper_asterisk115_request,
	devicestate:		sccp_wrapper_asterisk115_devicestate,
	send_digit_begin:	sccp_wrapper_recvdigit_begin,
	send_digit_end:		sccp_wrapper_recvdigit_end,
	call:			sccp_wrapper_asterisk115_call,
	hangup:			sccp_wrapper_asterisk115_hangup,
	answer:			sccp_wrapper_asterisk115_answer,
	read:			sccp_wrapper_asterisk115_rtp_read,
	write:			sccp_wrapper_asterisk115_rtp_write,
	send_text:		sccp_pbx_sendtext,
	send_image:		NULL,
	send_html:		sccp_pbx_sendHTML,
	exception:		NULL,
//	bridge:			sccp_wrapper_asterisk115_rtpBridge,
        bridge:			ast_rtp_instance_bridge,
	early_bridge:		NULL,
	indicate:		sccp_wrapper_asterisk115_indicate,
	fixup:			sccp_wrapper_asterisk115_fixup,
	setoption:		NULL,
	queryoption:		NULL,
	transfer:		NULL,
	write_video:		sccp_wrapper_asterisk115_rtp_write,
	write_text:		NULL,
	bridged_channel:	NULL,
	func_channel_read:	sccp_wrapper_asterisk_channel_read,
	func_channel_write:	sccp_asterisk_pbx_fktChannelWrite,
	get_base_channel:	NULL,
	set_base_channel:	NULL,
	/* *INDENT-ON* */
};

#else
/*!
 * \brief NULL Tech Structure
 */
static const struct ast_channel_tech null_tech = {
	.type = "NULL",
	.description = "Null channel (should not see this)",
};

/*!
 * \brief SCCP Tech Structure
 */
struct ast_channel_tech sccp_tech = {
	/* *INDENT-OFF* */
	.type 			= SCCP_TECHTYPE_STR,
	.description 		= "Skinny Client Control Protocol (SCCP)",
	// we could use the skinny_codec = ast_codec mapping here to generate the list of capabilities
//      .capabilities           = AST_FORMAT_SLINEAR16 | AST_FORMAT_SLINEAR | AST_FORMAT_ALAW | AST_FORMAT_ULAW | AST_FORMAT_GSM | AST_FORMAT_G723_1 | AST_FORMAT_G729A,
	.properties 		= AST_CHAN_TP_WANTSJITTER | AST_CHAN_TP_CREATESJITTER,
	.requester 		= sccp_wrapper_asterisk115_request,
	.devicestate 		= sccp_wrapper_asterisk115_devicestate,
	.call 			= sccp_wrapper_asterisk115_call,
	.hangup 		= sccp_wrapper_asterisk115_hangup,
	.answer 		= sccp_wrapper_asterisk115_answer,
	.read 			= sccp_wrapper_asterisk115_rtp_read,
	.write 			= sccp_wrapper_asterisk115_rtp_write,
	.write_video 		= sccp_wrapper_asterisk115_rtp_write,
	.indicate 		= sccp_wrapper_asterisk115_indicate,
	.fixup 			= sccp_wrapper_asterisk115_fixup,
	//.transfer 		= sccp_pbx_transfer,
#ifdef CS_AST_RTP_INSTANCE_BRIDGE
	.bridge 		= ast_rtp_instance_bridge,
//	.bridge 		= sccp_wrapper_asterisk115_rtpBridge,
#endif
	//.early_bridge		= ast_rtp_early_bridge,
	//.bridged_channel      =

	.send_text 		= sccp_pbx_sendtext,
	.send_html 		= sccp_pbx_sendHTML,
	//.send_image           =

	.func_channel_read 	= sccp_wrapper_asterisk_channel_read,
	.func_channel_write	= sccp_asterisk_pbx_fktChannelWrite,

	.send_digit_begin 	= sccp_wrapper_recvdigit_begin,
	.send_digit_end 	= sccp_wrapper_recvdigit_end,

	//.write_text           =
	//.write_video          =
	//.cc_callback          =                                              // ccss, new >1.6.0
	//.exception            =                                              // new >1.6.0
	//.setoption              = sccp_wrapper_asterisk115_setOption,
	//.queryoption          =                                              // new >1.6.0
	//.get_pvt_uniqueid     = sccp_pbx_get_callid,                         // new >1.6.0
	//.get_base_channel     =
	//.set_base_channel     =
	/* *INDENT-ON* */
};

#endif

static int sccp_wrapper_asterisk115_devicestate(const char *data)
{
	int res = AST_DEVICE_UNKNOWN;
	char *lineName = (char *) data;
	char *deviceId = NULL;
	sccp_channelstate_t state;

	if ((deviceId = strchr(lineName, '@'))) {
		*deviceId = '\0';
		deviceId++;
	}

	state = sccp_hint_getLinestate(lineName, deviceId);
	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "SCCP: (sccp_asterisk_devicestate) sccp_hint returned state:%s for '%s'\n", sccp_channelstate2str(state), lineName);
	switch (state) {
		case SCCP_CHANNELSTATE_DOWN:
		case SCCP_CHANNELSTATE_ONHOOK:
			res = AST_DEVICE_NOT_INUSE;
			break;
		case SCCP_CHANNELSTATE_RINGING:
			res = AST_DEVICE_RINGING;
			break;
		case SCCP_CHANNELSTATE_HOLD:
			res = AST_DEVICE_ONHOLD;
			break;
		case SCCP_CHANNELSTATE_INVALIDNUMBER:
			res = AST_DEVICE_INVALID;
			break;
		case SCCP_CHANNELSTATE_BUSY:
			res = AST_DEVICE_BUSY;
			break;
		case SCCP_CHANNELSTATE_DND:
			res = AST_DEVICE_BUSY;
			break;
		case SCCP_CHANNELSTATE_CONGESTION:
#ifndef CS_EXPERIMENTAL
		case SCCP_CHANNELSTATE_ZOMBIE:
		case SCCP_CHANNELSTATE_SPEEDDIAL:
		case SCCP_CHANNELSTATE_INVALIDCONFERENCE:
#endif
			res = AST_DEVICE_UNAVAILABLE;
			break;
		case SCCP_CHANNELSTATE_RINGOUT:
		case SCCP_CHANNELSTATE_RINGOUT_ALERTING:
#ifdef CS_EXPERIMENTAL
			res = AST_DEVICE_RINGINUSE;
			break;
#endif
		case SCCP_CHANNELSTATE_DIALING:
		case SCCP_CHANNELSTATE_DIGITSFOLL:
		case SCCP_CHANNELSTATE_PROGRESS:
		case SCCP_CHANNELSTATE_CALLWAITING:
#ifndef CS_EXPERIMENTAL
			res = AST_DEVICE_BUSY;
			break;
#endif
		case SCCP_CHANNELSTATE_CONNECTEDCONFERENCE:
		case SCCP_CHANNELSTATE_OFFHOOK:
		case SCCP_CHANNELSTATE_GETDIGITS:
		case SCCP_CHANNELSTATE_CONNECTED:
		case SCCP_CHANNELSTATE_PROCEED:
		case SCCP_CHANNELSTATE_BLINDTRANSFER:
		case SCCP_CHANNELSTATE_CALLTRANSFER:
		case SCCP_CHANNELSTATE_CALLCONFERENCE:
		case SCCP_CHANNELSTATE_CALLPARK:
		case SCCP_CHANNELSTATE_CALLREMOTEMULTILINE:
			res = AST_DEVICE_INUSE;
			break;
		case SCCP_CHANNELSTATE_SENTINEL:
#ifdef CS_EXPERIMENTAL
		case SCCP_CHANNELSTATE_SPEEDDIAL:
		case SCCP_CHANNELSTATE_INVALIDCONFERENCE:
		case SCCP_CHANNELSTATE_ZOMBIE:
			res = AST_DEVICE_UNKNOWN;
#endif
			break;
	}

	sccp_log((DEBUGCAT_HINT)) (VERBOSE_PREFIX_4 "SCCP: (sccp_asterisk_devicestate) PBX requests state for '%s' - state %s\n", lineName, ast_devstate2str(res));
	return res;
}

static boolean_t sccp_wrapper_asterisk115_setReadFormat(constChannelPtr channel, skinny_codec_t codec);

#define RTP_NEW_SOURCE(_c,_log) 								\
	if(c->rtp.audio.instance) { 										\
		ast_rtp_new_source(c->rtp.audio.instance); 							\
		sccp_log((DEBUGCAT_PBX | DEBUGCAT_INDICATE))(VERBOSE_PREFIX_3 "SCCP: " #_log "\n"); 	\
	}

#define RTP_CHANGE_SOURCE(_c,_log) 								\
	if(c->rtp.audio.instance) {										\
		ast_rtp_change_source(c->rtp.audio.instance);						\
		sccp_log((DEBUGCAT_PBX | DEBUGCAT_INDICATE))(VERBOSE_PREFIX_3 "SCCP: " #_log "\n"); 	\
	}

// static void get_skinnyFormats(struct ast_format_cap *format, skinny_codec_t codecs[], size_t size)
// {
//      unsigned int x;
//      unsigned len = 0;
//
//      size_t f_len;
//      struct ast_format tmp_fmt;
//      const struct ast_format_list *f_list = ast_format_list_get(&f_len);
//
//      if (!size) {
//              f_list = ast_format_list_destroy(f_list);
//              return;
//      }
//
//      for (x = 0; x < ARRAY_LEN(pbx2skinny_codec_maps) && len <= size; x++) {
//              ast_format_copy(&tmp_fmt, &f_list[x].format);
//              if (ast_format_cap_iscompatible(format, &tmp_fmt)) {
//                      if (pbx2skinny_codec_maps[x].pbx_codec == ((uint) tmp_fmt.id)) {
//                              codecs[len++] = pbx2skinny_codec_maps[x].skinny_codec;
//                      }
//              }
//      }
//      f_list = ast_format_list_destroy(f_list);
// }

/*************************************************************************************************************** CODEC **/

/*! \brief Get the name of a format
 * \note replacement for ast_getformatname
 * \param format id of format
 * \return A static string containing the name of the format or "unknown" if unknown.
 */
const char *pbx_getformatname(const struct ast_format *format)
{
	return ast_format_get_codec_name(format);
}

/*!
 * \brief Get the names of a set of formats
 * \note replacement for ast_getformatname_multiple
 * \param buf a buffer for the output string
 * \param size size of buf (bytes)
 * \param format the format (combined IDs of codecs)
 * Prints a list of readable codec names corresponding to "format".
 * ex: for format=AST_FORMAT_GSM|AST_FORMAT_SPEEX|AST_FORMAT_ILBC it will return "0x602 (GSM|SPEEX|ILBC)"
 * \return The return value is buf.
 */
const char *pbx_getformatname_multiple(char *buf, size_t size, struct ast_format_cap *format)
{
	struct ast_str *codec_buf = ast_str_alloca(64);
	ast_format_cap_get_names(format, &codec_buf);
	snprintf(buf, size, "%s", ast_format_cap_get_names(format, &codec_buf));
	return buf;
}


/*!
 * \brief Read from an Asterisk Channel
 * \param ast Asterisk Channel as ast_channel
 *
 * \called_from_asterisk
 *
 * \note not following the refcount rules... channel is already retained
 */
static PBX_FRAME_TYPE *sccp_wrapper_asterisk115_rtp_read(PBX_CHANNEL_TYPE * ast)
{
	//AUTO_RELEASE(sccp_channel_t, c , NULL);									// not following the refcount rules... channel is already retained
	sccp_channel_t *c = NULL;
	PBX_FRAME_TYPE *frame = &ast_null_frame;

	if (!(c = CS_AST_CHANNEL_PVT(ast))) {									// not following the refcount rules... channel is already retained
		pbx_log(LOG_ERROR, "SCCP: (rtp_read) no channel pvt\n");
		goto EXIT_FUNC;
	}

	if (!c->rtp.audio.instance) {
		pbx_log(LOG_NOTICE, "SCCP: (rtp_read) no rtp stream yet. skip\n");
		goto EXIT_FUNC;
	}

	switch (ast_channel_fdno(ast)) {

		case 0:
			frame = ast_rtp_instance_read(c->rtp.audio.instance, 0);					/* RTP Audio */
			break;
		case 1:
			frame = ast_rtp_instance_read(c->rtp.audio.instance, 1);					/* RTCP Control Channel */
			break;
#ifdef CS_SCCP_VIDEO
		case 2:
			frame = ast_rtp_instance_read(c->rtp.video.instance, 0);					/* RTP Video */
			break;
		case 3:
			frame = ast_rtp_instance_read(c->rtp.video.instance, 1);					/* RTCP Control Channel for video */
			break;
#endif
		default:
			break;
	}
	//sccp_log((DEBUGCAT_CORE))(VERBOSE_PREFIX_3 "%s: read format: ast->fdno: %d, frametype: %d, %s(%d)\n", DEV_ID_LOG(c->device), ast_channel_fdno(ast), frame->frametype, pbx_getformatname(frame->subclass), frame->subclass);
	if (frame->frametype == AST_FRAME_VOICE) {
#ifdef CS_SCCP_CONFERENCE
		if (c->conference && (!ast_format_cache_is_slinear(ast_channel_readformat(ast)))) {
			ast_set_read_format(ast, ast_format_slin96);
		}
#endif
	}

EXIT_FUNC:
	return frame;
}

/*!
 * \brief Find Asterisk/PBX channel by linkid
 *
 * \param ast   pbx channel
 * \param data  linkId as void *
 *
 * \return int
 */
static int pbx_find_channel_by_linkid(PBX_CHANNEL_TYPE * ast, const void *data)
{
	const char *linkedId = (char *) data;

	if (!linkedId) {
		return 0;
	}

	return !pbx_channel_pbx(ast) && ast_channel_linkedid(ast) && (!strcasecmp(ast_channel_linkedid(ast), linkedId)) && !pbx_channel_masq(ast);
}

static const char *asterisk_indication2str(int ind)
{
	switch (ind) {
		case AST_CONTROL_HANGUP: return "AST_CONTROL_HANGUP: Other end has hungup";
		case AST_CONTROL_RING: return "AST_CONTROL_RING: Local ring";
		case AST_CONTROL_RINGING: return "AST_CONTROL_RINGING: Remote end is ringing";
		case AST_CONTROL_ANSWER: return "AST_CONTROL_ANSWER: Remote end has answered";
		case AST_CONTROL_BUSY: return "AST_CONTROL_BUSY: Remote end is busy";
		case AST_CONTROL_TAKEOFFHOOK: return "AST_CONTROL_TAKEOFFHOOK: Make it go off hook";
		case AST_CONTROL_OFFHOOK: return "AST_CONTROL_OFFHOOK: Line is off hook";
		case AST_CONTROL_CONGESTION: return "AST_CONTROL_CONGESTION: Congestion (circuits busy)";
		case AST_CONTROL_FLASH: return "AST_CONTROL_FLASH: Flash hook";
		case AST_CONTROL_WINK: return "AST_CONTROL_WINK: Wink";
		case AST_CONTROL_OPTION: return "AST_CONTROL_OPTION: Set a low-level option";
		case AST_CONTROL_RADIO_KEY: return "AST_CONTROL_RADIO_KEY: Key Radio";
		case AST_CONTROL_RADIO_UNKEY: return "AST_CONTROL_RADIO_UNKEY: Un-Key Radio";
		case AST_CONTROL_PROGRESS: return "AST_CONTROL_PROGRESS: Indicate PROGRESS";
		case AST_CONTROL_PROCEEDING: return "AST_CONTROL_PROCEEDING: Indicate CALL PROCEEDING";
		case AST_CONTROL_HOLD: return "AST_CONTROL_HOLD: Indicate call is placed on hold";
		case AST_CONTROL_UNHOLD: return "AST_CONTROL_UNHOLD: Indicate call left hold";
		case AST_CONTROL_VIDUPDATE: return "AST_CONTROL_VIDUPDATE: Indicate video frame update";
		case _XXX_AST_CONTROL_T38: return "_XXX_AST_CONTROL_T38: T38 state change request/notification. Deprecated This is no longer supported. Use AST_CONTROL_T38_PARAMETERS instead.";
		case AST_CONTROL_SRCUPDATE: return "AST_CONTROL_SRCUPDATE: Indicate source of media has changed";
		case AST_CONTROL_TRANSFER: return "AST_CONTROL_TRANSFER: Indicate status of a transfer request";
		case AST_CONTROL_CONNECTED_LINE: return "AST_CONTROL_CONNECTED_LINE: Indicate connected line has changed";
		case AST_CONTROL_REDIRECTING: return "AST_CONTROL_REDIRECTING: Indicate redirecting id has changed";
		case AST_CONTROL_T38_PARAMETERS: return "AST_CONTROL_T38_PARAMETERS: T38 state change request/notification with parameters";
		case AST_CONTROL_CC: return "AST_CONTROL_CC: Indication that Call completion service is possible";
		case AST_CONTROL_SRCCHANGE: return "AST_CONTROL_SRCCHANGE: Media source has changed and requires a new RTP SSRC";
		case AST_CONTROL_READ_ACTION: return "AST_CONTROL_READ_ACTION: Tell ast_read to take a specific action";
		case AST_CONTROL_AOC: return "AST_CONTROL_AOC: Advice of Charge with encoded generic AOC payload";
		case AST_CONTROL_END_OF_Q: return "AST_CONTROL_END_OF_Q: Indicate that this position was the end of the channel queue for a softhangup.";
		case AST_CONTROL_INCOMPLETE: return "AST_CONTROL_INCOMPLETE: Indication that the extension dialed is incomplete";
		case AST_CONTROL_MCID: return "AST_CONTROL_MCID: Indicate that the caller is being malicious.";
		case AST_CONTROL_UPDATE_RTP_PEER: return "AST_CONTROL_UPDATE_RTP_PEER: Interrupt the bridge and have it update the peer";
		case AST_CONTROL_PVT_CAUSE_CODE: return "AST_CONTROL_PVT_CAUSE_CODE: Contains an update to the protocol-specific cause-code stored for branching dials";
		case AST_CONTROL_MASQUERADE_NOTIFY: return "AST_CONTROL_MASQUERADE_NOTIFY: A masquerade is about to begin/end."; // Never sent as a frame but directly with ast_indicate_data()
		case -1: return "AST_CONTROL_PROD: Kick remote channel";
	}
	return "Unknown/Unhandled/IAX Indication";
}

static int sccp_wrapper_asterisk115_indicate(PBX_CHANNEL_TYPE * ast, int ind, const void *data, size_t datalen)
{
	int res = 0;
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL | DEBUGCAT_INDICATE)) (VERBOSE_PREFIX_3 "SCCP: (pbx_indicate) start indicate '%s'\n", asterisk_indication2str(ind));

	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));
	if (!c) {
		//sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL | DEBUGCAT_INDICATE)) (VERBOSE_PREFIX_3 "SCCP: (pbx_indicate) no sccp channel yet\n");
		return -1;
	}

	AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));
	if (!d || c->state == SCCP_CHANNELSTATE_DOWN) {
		//sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL | DEBUGCAT_INDICATE)) (VERBOSE_PREFIX_3 "SCCP: (pbx_indicate) no sccp device yet\n");
		switch (ind) {
			case AST_CONTROL_CONNECTED_LINE:
				sccp_asterisk_connectedline(c, data, datalen);
				res = 0;
				break;
			case AST_CONTROL_REDIRECTING:
				sccp_asterisk_redirectedUpdate(c, data, datalen);
				res = 0;
				break;
			default:
				res = -1;
				break;
		}
		return res;
	}


	/* when the rtp media stream is open we will let asterisk emulate the tones */
	res = (((c->rtp.audio.receiveChannelState != SCCP_RTP_STATUS_INACTIVE) || (d && d->earlyrtp)) ? -1 : 0);
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL | DEBUGCAT_INDICATE)) (VERBOSE_PREFIX_3 "%s: (pbx_indicate) start indicate '%s' (%d) condition on channel %s (readStat:%d, receiveChannelState:%d, rtp:%s, default res:%s (%d))\n", DEV_ID_LOG(d), asterisk_indication2str(ind), ind, pbx_channel_name(ast), c->rtp.audio.receiveChannelState, c->rtp.audio.receiveChannelState, (c->rtp.audio.instance) ? "yes" : "no", res ? "inband signaling" : "outofband signaling", res);

	switch (ind) {
		case AST_CONTROL_RINGING:
			if (SKINNY_CALLTYPE_OUTBOUND == c->calltype) {
				// Allow signalling of RINGOUT only on outbound calls.
				// Otherwise, there are some issues with late arrival of ringing
				// indications on ISDN calls (chan_lcr, chan_dahdi) (-DD).
				sccp_indicate(d, c, SCCP_CHANNELSTATE_RINGOUT);
				if (d->earlyrtp == SCCP_EARLYRTP_IMMEDIATE) {
					/* 
					 * Redial button isnt't working properly in immediate mode, because the
					 * last dialed number was being remembered too early. This fix
					 * remembers the last dialed number in the same cases, where the dialed number
					 * is being sent - after receiving of RINGOUT -Pavel Troller
					 */
					AUTO_RELEASE(sccp_linedevices_t, linedevice , sccp_linedevice_find(d, c->line));
					if(linedevice){ 
						sccp_device_setLastNumberDialed(d, c->dialedNumber, linedevice);
					}
					sccp_wrapper_asterisk115_setDialedNumber(c, c->dialedNumber);
				}
				iPbx.set_callstate(c, AST_STATE_RING);

				struct ast_channel_iterator *iterator = ast_channel_iterator_all_new();

				((struct ao2_iterator *) iterator)->flags |= AO2_ITERATOR_DONTLOCK;
				/*! \todo handle multiple remotePeers i.e. DIAL(SCCP/400&SIP/300), find smallest common codecs, what order to use ? */
				PBX_CHANNEL_TYPE *remotePeer;

				for (; (remotePeer = ast_channel_iterator_next(iterator)); ast_channel_unref(remotePeer)) {
					if (pbx_find_channel_by_linkid(remotePeer, (void *) ast_channel_linkedid(ast))) {
						char buf[512];
						
						AUTO_RELEASE(sccp_channel_t, remoteSccpChannel , get_sccp_channel_from_pbx_channel(remotePeer));
						if (remoteSccpChannel) {
							sccp_codec_multiple2str(buf, sizeof(buf) - 1, remoteSccpChannel->preferences.audio, ARRAY_LEN(remoteSccpChannel->preferences.audio));
							sccp_log(DEBUGCAT_CODEC) (VERBOSE_PREFIX_4 "remote preferences: %s\n", buf);
							uint8_t x, y, z;

							z = 0;
							for (x = 0; x < SKINNY_MAX_CAPABILITIES && remoteSccpChannel->preferences.audio[x] != 0; x++) {
								for (y = 0; y < SKINNY_MAX_CAPABILITIES && remoteSccpChannel->capabilities.audio[y] != 0; y++) {
									if (remoteSccpChannel->preferences.audio[x] == remoteSccpChannel->capabilities.audio[y]) {
										c->remoteCapabilities.audio[z++] = remoteSccpChannel->preferences.audio[x];
									}
								}
							}
						} else {
							/* ask peer for it's codecs */
							//ast_rtp_instance_get_codecs(c->rtp.adio.rtp);	
							//ast_rtp_instance_get_codecs(c->rtp.video.instance);
							struct ast_str *codec_buf = ast_str_alloca(64);
							sccp_log(DEBUGCAT_CODEC) (VERBOSE_PREFIX_4 "remote nativeformats: %s\n", ast_format_cap_get_names(ast_channel_nativeformats(remotePeer), &codec_buf));
							sccp_asterisk115_getSkinnyFormatMultiple(ast_channel_nativeformats(remotePeer), c->remoteCapabilities.audio, ARRAY_LEN(c->remoteCapabilities.audio));
						}

						sccp_codec_multiple2str(buf, sizeof(buf) - 1, c->remoteCapabilities.audio, ARRAY_LEN(c->remoteCapabilities.audio));
						sccp_log(DEBUGCAT_CODEC) (VERBOSE_PREFIX_4 "remote caps: %s\n", buf);
						ast_channel_unref(remotePeer);
						break;
					}
				}
				ast_channel_iterator_destroy(iterator);
			}
			break;
		case AST_CONTROL_BUSY:
			sccp_indicate(d, c, SCCP_CHANNELSTATE_BUSY);
			iPbx.set_callstate(c, AST_STATE_BUSY);
			break;
		case AST_CONTROL_CONGESTION:
			sccp_indicate(d, c, SCCP_CHANNELSTATE_CONGESTION);
			break;
		case AST_CONTROL_PROGRESS:
			if (c->state != SCCP_CHANNELSTATE_CONNECTED && c->previousChannelState != SCCP_CHANNELSTATE_CONNECTED) {
				sccp_indicate(d, c, SCCP_CHANNELSTATE_PROGRESS);
			} else {
				// ORIGINATE() to SIP indicates PROGRESS after CONNECTED, causing issues with transfer
				sccp_indicate(d, c, SCCP_CHANNELSTATE_CONNECTED);
			}
			res = -1;
			break;
		case AST_CONTROL_PROCEEDING:
			if (d->earlyrtp == SCCP_EARLYRTP_IMMEDIATE) {
				/* 
					* Redial button isnt't working properly in immediate mode, because the
					* last dialed number was being remembered too early. This fix
					* remembers the last dialed number in the same cases, where the dialed number
					* is being sent - after receiving of PROCEEDING -Pavel Troller
					*/
				AUTO_RELEASE(sccp_linedevices_t, linedevice , sccp_linedevice_find(d, c->line));
				if(linedevice){ 
					sccp_device_setLastNumberDialed(d, c->dialedNumber, linedevice);
				}
				sccp_wrapper_asterisk115_setDialedNumber(c, c->dialedNumber);
			}
			sccp_indicate(d, c, SCCP_CHANNELSTATE_PROCEED);
			res = -1;
			break;
		case AST_CONTROL_SRCCHANGE:									/* ask our channel's remote source address to update */
			if (c->rtp.audio.instance) {
				ast_rtp_instance_change_source(c->rtp.audio.instance);
			}
			res = 0;
			break;

		case AST_CONTROL_SRCUPDATE:									/* semd control bit to force other side to update, their source address */
			/* Source media has changed. */
			sccp_log((DEBUGCAT_PBX | DEBUGCAT_INDICATE)) (VERBOSE_PREFIX_3 "SCCP: Source UPDATE request\n");

			if (c->rtp.audio.instance) {
				ast_rtp_instance_change_source(c->rtp.audio.instance);
			}
			res = 0;
			break;

			/* when the bridged channel hold/unhold the call we are notified here */
		case AST_CONTROL_HOLD:
			sccp_asterisk_moh_start(ast, (const char *) data, c->musicclass);
			res = 0;
			break;
		case AST_CONTROL_UNHOLD:
			sccp_asterisk_moh_stop(ast);

			if (c->rtp.audio.instance) {
				ast_rtp_instance_update_source(c->rtp.audio.instance);
			}
			res = 0;
			break;

		case AST_CONTROL_CONNECTED_LINE:
			/* remarking out this code, as it is causing issues with callforward + FREEPBX,  the calling party will not hear the remote end ringing
			 this patch was added to suppress 'double callwaiting tone', but channel PROD(-1) below is taking care of that already
			*/
			//if (c->calltype == SKINNY_CALLTYPE_OUTBOUND && c->rtp.audio.receiveChannelState == SCCP_RTP_STATUS_INACTIVE && c->state > SCCP_CHANNELSTATE_DIALING) {
			//	sccp_channel_openReceiveChannel(c);
			//}
			sccp_asterisk_connectedline(c, data, datalen);
			res = 0;
			break;

		case AST_CONTROL_TRANSFER:
			ast_log(LOG_NOTICE, "%s: Ast Control Transfer: %d", c->designator, *(int *)data);
			//sccp_asterisk_connectedline(c, data, datalen);
			res = 0;
			break;

		case AST_CONTROL_REDIRECTING:
			sccp_asterisk_redirectedUpdate(c, data, datalen);
			sccp_indicate(d, c, c->state);
			res = 0;
			break;

		case AST_CONTROL_VIDUPDATE:									/* Request a video frame update */
#ifdef CS_SCCP_VIDEO
			if (c->rtp.video.instance && d && sccp_device_isVideoSupported(d) && c->videomode != SCCP_VIDEO_MODE_OFF) {
				d->protocol->sendFastPictureUpdate(d, c);
				res = 0;
			} else 
#endif
			{
				res = -1;
			}
			break;
#ifdef CS_AST_CONTROL_INCOMPLETE
		case AST_CONTROL_INCOMPLETE:									/*!< Indication that the extension dialed is incomplete */
			/* \todo implement dial continuation by:
			 *  - display message incomplete number
			 *  - adding time to channel->scheduler.digittimeout
			 *  - rescheduling sccp_pbx_sched_dial
			 */
			/*
			if (d->earlyrtp != SCCP_EARLYRTP_IMMEDIATE) {
				if (!c->scheduler.deny) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_DIGITSFOLL);
					//sccp_channel_schedule_digittimout(c, c->enbloc.digittimeout);
					sccp_channel_schedule_digittimout(c, c->scheduler.digittimeout);
				} else {
					sccp_channel_stop_schedule_digittimout(c);
					sccp_indicate(d, c, SCCP_CHANNELSTATE_ONHOOK);
				}
			}
			*/
			res = 0;
			break;
#endif
		case AST_CONTROL_PVT_CAUSE_CODE:
			{
				/*! \todo This would also be a good moment to update the c->requestHangup to requestQueueHangup */
				int hangupcause = ast_channel_hangupcause(ast);

				sccp_log((DEBUGCAT_PBX | DEBUGCAT_INDICATE)) (VERBOSE_PREFIX_3 "%s: hangup cause set: %d\n", c->designator, hangupcause);
			}
			res = -1;
			break;
		case AST_CONTROL_MASQUERADE_NOTIFY:
			res = -1;
			break;
		case -1:											// Asterisk prod the channel
			if (	c->line && 
				c->state > SCCP_GROUPED_CHANNELSTATE_DIALING && 
				c->calltype == SKINNY_CALLTYPE_OUTBOUND && 
				c->rtp.audio.receiveChannelState == SCCP_RTP_STATUS_INACTIVE &&
				!ast_channel_hangupcause(ast)
			) {
				sccp_channel_openReceiveChannel(c);
				uint8_t instance = sccp_device_find_index_for_line(d, c->line->name);
				sccp_dev_stoptone(d, instance, c->callid);
			}
			res = -1;
			break;
		default:
			sccp_log((DEBUGCAT_PBX | DEBUGCAT_INDICATE)) (VERBOSE_PREFIX_3 "%s: (pbx_indicate) Don't know how to indicate condition '%s' (%d)\n", DEV_ID_LOG(d), asterisk_indication2str(ind), ind);
			break;
	}
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_INDICATE)) (VERBOSE_PREFIX_2 "%s: (pbx_indicate) finish: send indication '%s' (%d)\n", DEV_ID_LOG(d), res ? "inband signaling" : "outofband signaling", res);
	return res;
}

/*!
 * \brief Write to an Asterisk Channel
 * \param ast Channel as ast_channel
 * \param frame Frame as ast_frame
 *
 * \called_from_asterisk
 *
 * \note not following the refcount rules... channel is already retained
 */
static int sccp_wrapper_asterisk115_rtp_write(PBX_CHANNEL_TYPE * ast, PBX_FRAME_TYPE * frame)
{
	//AUTO_RELEASE(sccp_channel_t, c , NULL);								// not following the refcount rules... channel is already retained
	sccp_channel_t *c = NULL;

	int res = 0;

	if (!(c = CS_AST_CHANNEL_PVT(ast))) {									// not following the refcount rules... channel is already retained
		return -1;
	}

	switch (frame->frametype) {
		case AST_FRAME_VOICE:
			// checking for samples to transmit
			if (!frame->samples) {
				if (strcasecmp(frame->src, "ast_prod")) {
					pbx_log(LOG_ERROR, "%s: Asked to transmit frame type %d with no samples.\n", c->currentDeviceId, (int) frame->frametype);
				} else {
					// frame->samples == 0  when frame_src is ast_prod
					sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "%s: Asterisk prodded channel %s.\n", c->currentDeviceId, pbx_channel_name(ast));
				}
			}
			if (c->rtp.audio.instance) {
				res = ast_rtp_instance_write(c->rtp.audio.instance, frame);
			}
			break;
		case AST_FRAME_IMAGE:
		case AST_FRAME_VIDEO:
#ifdef CS_SCCP_VIDEO
			if (c->rtp.video.receiveChannelState == SCCP_RTP_STATUS_INACTIVE && c->rtp.video.instance && c->state != SCCP_CHANNELSTATE_HOLD) {
				// int codec = pbx_codec2skinny_codec((frame->subclass.codec & AST_FORMAT_VIDEO_MASK));
				// int codec = pbx_codec2skinny_codec(frame->subclass.format.id);

				if (ast_format_cmp(ast_format_h264, frame->subclass.format) == AST_FORMAT_CMP_EQUAL) {
					sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: got video frame %s\n", c->currentDeviceId, "H264");
					c->rtp.video.writeFormat = SKINNY_CODEC_H264;
					sccp_channel_openMultiMediaReceiveChannel(c);
				}
			}

			if (c->rtp.video.instance && (c->rtp.video.receiveChannelState & SCCP_RTP_STATUS_ACTIVE) != 0) {
				res = ast_rtp_instance_write(c->rtp.video.instance, frame);
			}
#endif
			break;
		case AST_FRAME_TEXT:
		case AST_FRAME_MODEM:
		default:
			pbx_log(LOG_WARNING, "%s: Can't send %d type frames with SCCP write on channel %s\n", c->currentDeviceId, frame->frametype, pbx_channel_name(ast));
			break;
	}
	return res;
}

static void sccp_wrapper_asterisk115_setDialedNumber(const sccp_channel_t *channel, const char *number)
{
	PBX_CHANNEL_TYPE *pbx_channel = channel->owner;
	if (pbx_channel && number) {
		struct ast_party_dialed dialed;
		ast_party_dialed_init(&dialed);
		dialed.number.str = pbx_strdupa(number);
		ast_trim_blanks(dialed.number.str);
		ast_party_dialed_set(ast_channel_dialed(pbx_channel), &dialed);
	}
}

static void sccp_wrapper_asterisk115_setCalleridPresentation(PBX_CHANNEL_TYPE *pbx_channel, sccp_callerid_presentation_t presentation)
{
	if (pbx_channel && CALLERID_PRESENTATION_FORBIDDEN == presentation) {
		ast_channel_caller(pbx_channel)->id.name.presentation |= AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED;
		ast_channel_caller(pbx_channel)->id.number.presentation |= AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED;
	}
}

static int sccp_wrapper_asterisk115_setNativeAudioFormats(constChannelPtr channel, skinny_codec_t codec[], int length)
{
	int i;
	if (!channel || !channel->owner || !ast_channel_nativeformats(channel->owner)) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_wrapper_asterisk111_setNativeAudioFormats) no channel provided!\n");
		return 0;
	}

	length = 1;												//set only one codec

	ast_debug(10, "%s: set native Formats length: %d\n", (char *) channel->currentDeviceId, length);

	ast_format_cap_remove_by_type(ast_channel_nativeformats(channel->owner), AST_MEDIA_TYPE_AUDIO);
	//pbx_format_cap_append_skinny(ast_channel_nativeformats(channel->owner), codec);			// if length != 1
 	for (i = 0; i < length; i++) {
		struct ast_format *format = sccp_asterisk115_skinny2ast_format(codec[1]);
		if (format != ast_format_none) {
			unsigned int framing = ast_format_get_default_ms(format);
			ast_format_cap_append(ast_channel_nativeformats(channel->owner), format, framing);
		}
	}

	return 1;
}

static int sccp_wrapper_asterisk115_setNativeVideoFormats(constChannelPtr channel, skinny_codec_t codec)
{
	int i;
	int length = 1;

	for (i = 0; i < length; i++) {
		struct ast_format *format = sccp_asterisk115_skinny2ast_format(codec);
		if (format != ast_format_none) {
			unsigned int framing = ast_format_get_default_ms(format);
			ast_format_cap_append(ast_channel_nativeformats(channel->owner), format, framing);
		}
	}
	return 1;
}

static void sccp_wrapper_asterisk115_removeTimingFD(PBX_CHANNEL_TYPE *ast)
{
	if (ast) {
		struct ast_timer *timer=ast_channel_timer(ast);
		if (timer) {
			//ast_log(LOG_NOTICE, "%s: (clean_timer_fds) timername: %s, fd:%d\n", ast_channel_name(ast), ast_timer_get_name(timer), ast_timer_fd(timer));
			ast_timer_disable_continuous(timer);
			ast_timer_close(timer);
			ast_channel_set_fd(ast, AST_TIMING_FD, -1);
			ast_channel_timingfd_set(ast, -1);
			ast_channel_timer_set(ast, NULL);
		}
	}
}

static void sccp_wrapper_asterisk115_setOwner(sccp_channel_t * channel, PBX_CHANNEL_TYPE * pbx_channel)
{
	PBX_CHANNEL_TYPE *prev_owner = channel->owner;

	if (pbx_channel) {
		channel->owner = ast_channel_ref(pbx_channel);
	} else {
		channel->owner = NULL;
	}
	if (prev_owner) {
		ast_channel_unref(prev_owner);
	}
	if (channel->rtp.audio.instance) {
		ast_rtp_instance_set_channel_id(channel->rtp.audio.instance, pbx_channel ? ast_channel_uniqueid(pbx_channel) : "");
	}
	if (channel->rtp.video.instance) {
		ast_rtp_instance_set_channel_id(channel->rtp.video.instance, pbx_channel ? ast_channel_uniqueid(pbx_channel) : "");
	}
}

static void __sccp_asterisk115_updateConnectedLine(PBX_CHANNEL_TYPE *pbx_channel, const char *number, const char *name, uint8_t reason)
{
	if (!pbx_channel) {
		return;
	}

	struct ast_party_connected_line connected;
	struct ast_set_party_connected_line update_connected = {{0}};

	ast_party_connected_line_init(&connected);
	if (number) {
		update_connected.id.number = 1;
		connected.id.number.valid = 1;
		connected.id.number.str = pbx_strdupa(number);
		connected.id.number.presentation = AST_PRES_ALLOWED_NETWORK_NUMBER;
	}
	if (name) {
		update_connected.id.name = 1;
		connected.id.name.valid = 1;
		connected.id.name.str = pbx_strdupa(name);
		connected.id.name.presentation = AST_PRES_ALLOWED_NETWORK_NUMBER;
	}
	if (update_connected.id.number || update_connected.id.name) {
		ast_set_party_id_all(&update_connected.priv);
		// connected.id.tag = NULL;
		connected.source = reason;
		ast_channel_queue_connected_line_update(pbx_channel, &connected, &update_connected);
		sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: do connected line for line '%s', name: %s ,num: %s\n", pbx_channel_name(pbx_channel), name ? name : "(NULL)", number ? number : "(NULL)");
	}
}

static boolean_t sccp_wrapper_asterisk115_allocPBXChannel(sccp_channel_t * channel, const void *ids, const PBX_CHANNEL_TYPE * pbxSrcChannel, PBX_CHANNEL_TYPE ** _pbxDstChannel)
{
	const struct ast_assigned_ids *assignedids = NULL;
	PBX_CHANNEL_TYPE *pbxDstChannel = NULL;
	if (!channel || !channel->line) {
		return FALSE;
	}
	AUTO_RELEASE(sccp_line_t, line , sccp_line_retain(channel->line));
	if (!line) {
		return FALSE;
	}

	if (ids) {
		assignedids = ids;
	}

	sccp_log(DEBUGCAT_CHANNEL)(VERBOSE_PREFIX_3 "SCCP: (allocPBXChannel) Create New Channel with name: SCCP/%s-%08X\n", line->name, channel->callid);
	pbxDstChannel = ast_channel_alloc(0, AST_STATE_DOWN, line->cid_num, line->cid_name, line->accountcode, line->name, line->context, assignedids, pbxSrcChannel, line->amaflags, "%s", channel->designator);
	if (pbxDstChannel == NULL) {
		pbx_log(LOG_ERROR, "SCCP: (allocPBXChannel) ast_channel_alloc failed\n");
		return FALSE;
	}
	ast_channel_stage_snapshot(pbxDstChannel);
	sccp_wrapper_asterisk115_setOwner(channel, pbxDstChannel);

	ast_channel_tech_set(pbxDstChannel, &sccp_tech);
	ast_channel_tech_pvt_set(pbxDstChannel, sccp_channel_retain(channel));

	/* Copy Codec from SrcChannel */
	struct ast_format_cap *caps = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!caps) {
		return NULL;
	}

	if (pbxSrcChannel && ast_format_cap_count(ast_channel_nativeformats(pbxSrcChannel)) > 0) {
		ast_format_cap_append_from_cap(caps, ast_channel_nativeformats(pbxSrcChannel), AST_MEDIA_TYPE_UNKNOWN);
	} else if (line && channel) {
		pbx_format_cap_append_skinny(caps, channel->preferences.audio);
#ifdef CS_SCCP_VIDEO
		pbx_format_cap_append_skinny(caps, channel->preferences.video);
#endif		
	}
	ast_channel_nativeformats_set(pbxDstChannel, caps);
	ao2_ref(caps, -1);
	
	struct ast_format *tmpfmt = ast_format_cap_get_format(ast_channel_nativeformats(pbxDstChannel), 0);
	//struct ast_str *codec_buf = ast_str_alloca(AST_FORMAT_CAP_NAMES_LEN);
	//pbx_log(LOG_NOTICE, "allocPBXChannel: tmp->nativeformats=%s fmt=%s\n", ast_format_cap_get_names(ast_channel_nativeformats(pbxDstChannel), &codec_buf), ast_format_get_name(tmpfmt));
	ast_channel_set_writeformat(pbxDstChannel, tmpfmt);
	ast_channel_set_rawwriteformat(pbxDstChannel, tmpfmt);
	ast_channel_set_readformat(pbxDstChannel, tmpfmt);
	ast_channel_set_rawreadformat(pbxDstChannel, tmpfmt);
	ao2_ref(tmpfmt, -1);
	/* EndCodec */

	ast_channel_context_set(pbxDstChannel, line->context);
	ast_channel_exten_set(pbxDstChannel, line->name);
	ast_channel_priority_set(pbxDstChannel, 1);
	ast_channel_adsicpe_set(pbxDstChannel, AST_ADSI_UNAVAILABLE);

	if (!sccp_strlen_zero(line->language)) {
		ast_channel_language_set(pbxDstChannel, line->language);
	}

	if (!sccp_strlen_zero(line->accountcode)) {
		ast_channel_accountcode_set(pbxDstChannel, line->accountcode);
	}

	if (!sccp_strlen_zero(line->musicclass)) {
		ast_channel_musicclass_set(pbxDstChannel, line->musicclass);
	}

	if (line->amaflags) {
		ast_channel_amaflags_set(pbxDstChannel, line->amaflags);
	}
	if (line->callgroup) {
		ast_channel_callgroup_set(pbxDstChannel, line->callgroup);
	}

	ast_channel_callgroup_set(pbxDstChannel, line->callgroup);						// needed for ast_pickup_call
#if CS_SCCP_PICKUP
	if (line->pickupgroup) {
		ast_channel_pickupgroup_set(pbxDstChannel, line->pickupgroup);
	}
#endif
#if CS_AST_HAS_NAMEDGROUP
	if (!sccp_strlen_zero(line->namedcallgroup)) {
		ast_channel_named_callgroups_set(pbxDstChannel, ast_get_namedgroups(line->namedcallgroup));
	}

	if (!sccp_strlen_zero(line->namedpickupgroup)) {
		ast_channel_named_pickupgroups_set(pbxDstChannel, ast_get_namedgroups(line->namedpickupgroup));
	}
#endif
	if (!sccp_strlen_zero(line->parkinglot)) {
		ast_channel_parkinglot_set(pbxDstChannel, line->parkinglot);
	}

	/** the the tonezone using language information */
	if (!sccp_strlen_zero(line->language) && ast_get_indication_zone(line->language)) {
		ast_channel_zone_set(pbxDstChannel, ast_get_indication_zone(line->language));			/* this will core asterisk on hangup */
	}

	ast_module_ref(ast_module_info->self);
	ast_channel_stage_snapshot_done(pbxDstChannel);
	ast_channel_unlock(pbxDstChannel);

	(*_pbxDstChannel) = pbxDstChannel;

	return TRUE;
}

/*!
 * \brief Uses ast_channel_move instead of the old masquerade procedure to force a channel into a bridge and hangup the peer channel
 */
static boolean_t sccp_wrapper_asterisk115_masqueradeHelper(PBX_CHANNEL_TYPE * pbxChannel, PBX_CHANNEL_TYPE * pbxTmpChannel)
{
	boolean_t res = FALSE;
	pbx_log(LOG_NOTICE, "SCCP: (masqueradeHelper) answer temp: %s\n", ast_channel_name(pbxTmpChannel));

	ast_raw_answer(pbxTmpChannel);
//	ast_cdr_reset(ast_channel_name(pbxTmpChannel), 0);
	pbx_log(LOG_NOTICE, "SCCP: (masqueradeHelper) replace pbxTmpChannel: %s with %s (move)\n", ast_channel_name(pbxTmpChannel), ast_channel_name(pbxChannel));
	if (!ast_channel_move(pbxTmpChannel, pbxChannel)) {
		pbx_log(LOG_NOTICE, "SCCP: (masqueradeHelper) move succeeded. Hanging up orphan: %s\n", ast_channel_name(pbxChannel));
		/* Chan is now an orphaned zombie.  Destroy it. */
		if (pbx_test_flag(pbx_channel_flags(pbxChannel), AST_FLAG_BLOCKING)) {
			ast_softhangup(pbxChannel, AST_SOFTHANGUP_DEV);
		} else {
			ast_hangup(pbxChannel);
		}
		res = TRUE;
	} else {
		ast_hangup(pbxTmpChannel);
	}
	pbx_log(LOG_NOTICE, "SCCP: (masqueradeHelper) remove reference from pbxTmpChannel: %s\n", ast_channel_name(pbxTmpChannel));
	pbxTmpChannel = ast_channel_unref(pbxTmpChannel);
	return res;
}

static boolean_t sccp_wrapper_asterisk115_allocTempPBXChannel(PBX_CHANNEL_TYPE * pbxSrcChannel, PBX_CHANNEL_TYPE ** _pbxDstChannel)
{
	PBX_CHANNEL_TYPE *pbxDstChannel = NULL;
	struct ast_assigned_ids assignedids = {NULL, NULL};

	struct ast_format *ast_format;
	unsigned int framing;

	if (!pbxSrcChannel) {
		pbx_log(LOG_ERROR, "SCCP: (alloc_conferenceTempPBXChannel) no pbx channel provided\n");
		return FALSE;
	}
/*
	assignedids.uniqueid = ast_channel_uniqueid(pbxSrcChannel);
	{
		char *uniqueid2;
		uniqueid2 = ast_alloca(strlen(assignedids.uniqueid) + 3);
		strcpy(uniqueid2, assignedids.uniqueid);
		strcat(uniqueid2, ";2");
		assignedids.uniqueid2 = uniqueid2;
	}
*/

	ast_channel_lock(pbxSrcChannel);
	pbxDstChannel = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, ast_channel_accountcode(pbxSrcChannel), pbx_channel_exten(pbxSrcChannel), pbx_channel_context(pbxSrcChannel), &assignedids, pbxSrcChannel, ast_channel_amaflags(pbxSrcChannel), "%s-TMP", ast_channel_name(pbxSrcChannel));
	if (pbxDstChannel == NULL) {
		pbx_log(LOG_ERROR, "SCCP: (alloc_conferenceTempPBXChannel) ast_channel_alloc failed\n");
		return FALSE;
	}

	ast_channel_stage_snapshot(pbxDstChannel);
	// ast_channel_tech_set(pbxDstChannel, &sccp_tech);
	ast_channel_tech_set(pbxDstChannel, &null_tech);							// USE null_tech to prevent fixup issues. Channel is only used to masquerade a channel out of a running bridge.

	// if (ast_format_cap_is_empty(pbx_channel_nativeformats(pbxSrcChannel))) {
	if (ast_format_cap_count(pbx_channel_nativeformats(pbxSrcChannel)) == 0) {
		ast_format = ast_format_alaw;
		ao2_ref(ast_format, +1);
	} else {
		ast_format = ast_format_cap_get_best_by_type(pbx_channel_nativeformats(pbxSrcChannel), AST_MEDIA_TYPE_AUDIO);
	}
	framing = ast_format_get_default_ms(ast_format);
	ast_format_cap_remove_by_type(ast_channel_nativeformats(pbxDstChannel), AST_MEDIA_TYPE_AUDIO);
	ast_format_cap_append(ast_channel_nativeformats(pbxDstChannel), ast_format, framing);
	ast_set_read_format(pbxDstChannel, ast_format);
	ast_set_write_format(pbxDstChannel, ast_format);
	ao2_ref(ast_format, -1);

	ast_channel_context_set(pbxDstChannel, ast_channel_context(pbxSrcChannel));
	ast_channel_exten_set(pbxDstChannel, ast_channel_exten(pbxSrcChannel));
	ast_channel_priority_set(pbxDstChannel, ast_channel_priority(pbxSrcChannel));
	ast_channel_adsicpe_set(pbxDstChannel, AST_ADSI_UNAVAILABLE);
	ast_channel_stage_snapshot_done(pbxDstChannel);
	// ast_module_ref(ast_module_info->self);
	ast_channel_unlock(pbxSrcChannel);
	ast_channel_unlock(pbxDstChannel);

	(*_pbxDstChannel) = pbx_channel_ref(pbxDstChannel);
	return TRUE;
}

static PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_requestAnnouncementChannel(pbx_format_type format_type, const PBX_CHANNEL_TYPE * requestor, void *data)
{
	PBX_CHANNEL_TYPE *chan;
	int cause;
	struct ast_format_cap *cap;
	struct ast_format *ast_format;
	unsigned int framing;

	cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!cap) {
		return NULL;
	}
	// TODO(dkgroot): convert format_type to ast_format
	ast_format = ast_format_alaw;
	framing = ast_format_get_default_ms(ast_format);

	ast_format_cap_append(cap, ast_format, framing);
	chan = ast_request("SCCPCBAnn", cap, NULL, NULL, (char *) data, &cause);
	ao2_ref(cap, -1);

	if (!chan) {
		pbx_log(LOG_ERROR, "SCCP: Requested Unreal channel could not be created, cause: %d\n", cause);
	 	return NULL;
	}
	/* To make sure playback_chan has the same language of that profile */
	if (requestor) {
		ast_channel_lock(chan);
		ast_channel_language_set(chan, ast_channel_language(requestor));
		ast_channel_unlock(chan);
	}

	ast_debug(1, "Created Unreal channel '%s' related to '%s'\n", ast_channel_name(chan), (char *) data);
	return chan;
}


int sccp_wrapper_asterisk115_hangup(PBX_CHANNEL_TYPE * ast_channel)
{
	// ast_channel_stage_snapshot(ast_channel);
	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast_channel));
	int callid_created = 0;
	int res = -1;

	ast_callid callid = ast_channel_callid(ast_channel);

	if (c) {
		callid_created = c->pbx_callid_created;
		c->pbx_callid_created = 0;
		if (pbx_channel_hangupcause(ast_channel) == AST_CAUSE_ANSWERED_ELSEWHERE) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: This call was answered elsewhere\n");
			c->answered_elsewhere = TRUE;
		}
		/* postponing ast_channel_unref to sccp_channel destructor */
		AUTO_RELEASE(sccp_channel_t, channel , sccp_pbx_hangup(c));					/* explicit release from unretained channel returned by sccp_pbx_hangup */
		ast_channel_tech_pvt_set(ast_channel, NULL);
		(void) channel;											// suppress unused variable warning
	} else {												// after this moment c might have gone already
		ast_channel_tech_pvt_set(ast_channel, NULL);
		pbx_channel_unref(ast_channel);									// strange unknown channel, why did we get called to hang it up ?
	}
	if (callid_created) {
		ast_callid_threadstorage_auto_clean(callid, callid_created);
	}

	ast_module_unref(ast_module_info->self);
	// ast_channel_stage_snapshot_done(ast_channel);
	return res;
}

/*!
 * \brief Parking Thread Arguments Structure
 */

/*!
 * \brief Park the bridge channel of hostChannel
 * This function prepares the host and the bridged channel to be ready for parking.
 * It clones the pbx channel of both sides forward them to the park_thread
 *
 * \param hostChannel initial channel that request the parking
 * \todo we have a codec issue after unpark a call
 * \todo copy connected line info
 *
 */
static sccp_parkresult_t sccp_wrapper_asterisk115_park(constChannelPtr hostChannel)
{
	sccp_parkresult_t res = PARK_RESULT_FAIL;
	char extout[AST_MAX_EXTENSION] = "";
	RAII(struct ast_bridge_channel *, bridge_channel, NULL, ao2_cleanup);
	
	if (!ast_parking_provider_registered()) {
		return res;
	}

	AUTO_RELEASE(sccp_device_t, device , sccp_channel_getDevice(hostChannel));
	if (device && (ast_channel_state(hostChannel->owner) ==  AST_STATE_UP)) {
		ast_channel_lock(hostChannel->owner);								/* we have to lock our channel, otherwise asterisk crashes internally */
		bridge_channel = ast_channel_get_bridge_channel(hostChannel->owner);
		ast_channel_unlock(hostChannel->owner);
		if (bridge_channel) {
			if (!ast_parking_park_call(bridge_channel, extout, sizeof(extout))) {			/* new asterisk-12/13 implementation returns the parkext not the parking_space */
														/* See: https://issues.asterisk.org/jira/browse/ASTERISK-26029 */
				char extstr[20];
				memset(extstr, 0, sizeof(extstr));
				//snprintf(extstr, sizeof(extstr), "%c%c %.16s", 128, SKINNY_LBL_CALL_PARK_AT, extout);
				//sccp_dev_displayprinotify(device, extstr, 1, 10);				/* suppressing output of wrong parking information */
				sccp_dev_displayprinotify(device, SKINNY_DISP_CALL_PARK, 1, 10);
				sccp_log((DEBUGCAT_CHANNEL | DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Parked channel %s on %s\n", DEV_ID_LOG(device), ast_channel_name(hostChannel->owner), extout);
				res = PARK_RESULT_SUCCESS;
			}
		}
	}
	return res;
}

static boolean_t sccp_wrapper_asterisk115_getFeatureExtension(constChannelPtr channel, const char *featureName, char extension[SCCP_MAX_EXTENSION])
{
	int feat_res = ast_get_feature(channel->owner, featureName, extension, SCCP_MAX_EXTENSION);
	return feat_res ? FALSE : TRUE;
}

static boolean_t sccp_wrapper_asterisk115_getPickupExtension(constChannelPtr channel, char extension[SCCP_MAX_EXTENSION])
{
	boolean_t res = FALSE;
	struct ast_features_pickup_config *pickup_cfg = NULL;

	if (channel->owner) {
		pbx_channel_lock(channel->owner);
		pickup_cfg = ast_get_chan_features_pickup_config(channel->owner);
		if (!pickup_cfg) {
			ast_log(LOG_ERROR, "Unable to retrieve pickup configuration options. Unable to detect call pickup extension\n");
		} else {
			if (!sccp_strlen_zero(pickup_cfg->pickupexten)) {
				sccp_copy_string(extension, pickup_cfg->pickupexten, SCCP_MAX_EXTENSION);
				res = TRUE;
			}
			ast_channel_unref(pickup_cfg);
		}
		pbx_channel_unlock(channel->owner);
	}
	return res;
}

static uint8_t sccp_wrapper_asterisk115_get_payloadType(const struct sccp_rtp *rtp, skinny_codec_t codec)
{
	struct ast_format *astCodec = sccp_asterisk115_skinny2ast_format(codec);
	if (astCodec != ast_format_none) {
		return ast_rtp_codecs_payload_code(ast_rtp_instance_get_codecs(rtp->instance), skinny_codec2pbx_codec(codec), astCodec, 0);
	}
	return 0;
}

static int sccp_wrapper_asterisk115_get_sampleRate(skinny_codec_t codec)
{
	struct ast_format *astCodec = sccp_asterisk115_skinny2ast_format(codec);
	if (astCodec != ast_format_none) {
		return ast_rtp_lookup_sample_rate2(1, astCodec, 0);
	}
	return 0;
}

static sccp_extension_status_t sccp_wrapper_asterisk115_extensionStatus(constChannelPtr channel)
{
	PBX_CHANNEL_TYPE *pbx_channel = channel->owner;

	if (!pbx_channel || !pbx_channel_context(pbx_channel)) {
		pbx_log(LOG_ERROR, "%s: (extension_status) Either no pbx_channel or no valid context provided to lookup number\n", channel->designator);
		return SCCP_EXTENSION_NOTEXISTS;
	}
	int ignore_pat = ast_ignore_pattern(pbx_channel_context(pbx_channel), channel->dialedNumber);
	int ext_exist = ast_exists_extension(pbx_channel, pbx_channel_context(pbx_channel), channel->dialedNumber, 1, channel->line->cid_num);
	int ext_canmatch = ast_canmatch_extension(pbx_channel, pbx_channel_context(pbx_channel), channel->dialedNumber, 1, channel->line->cid_num);
	int ext_matchmore = ast_matchmore_extension(pbx_channel, pbx_channel_context(pbx_channel), channel->dialedNumber, 1, channel->line->cid_num);

	// RAII(struct ast_features_pickup_config *, pickup_cfg, ast_get_chan_features_pickup_config(pbx_channel), ao2_cleanup);
	// const char *pickupexten = (pickup_cfg) ? pickup_cfg->pickupexten : "-";

	/* if we dialed the pickup extention, mark this as exact match */
	const char *pickupexten = "";
	struct ast_features_pickup_config *pickup_cfg = NULL;

	if (channel->owner) {
		pbx_channel_lock(channel->owner);
		pickup_cfg = ast_get_chan_features_pickup_config(channel->owner);
		if (!pickup_cfg) {
			ast_log(LOG_ERROR, "Unable to retrieve pickup configuration options. Unable to detect call pickup extension\n");
			pickupexten = "";
		} else {
			pickupexten = pbx_strdupa(pickup_cfg->pickupexten);
			ast_channel_unref(pickup_cfg);
		}
		pbx_channel_unlock(channel->owner);
	}
	if (!sccp_strlen_zero(pickupexten) && sccp_strcaseequals(pickupexten, channel->dialedNumber)) {
		sccp_log(DEBUGCAT_CORE) (VERBOSE_PREFIX_3 "SCCP: pbx extension matcher found pickup extension %s matches dialed number %s\n", channel->dialedNumber, pickupexten);
		ext_exist = 1;
		ext_canmatch = 1;
		ext_matchmore = 0;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "+- pbx extension matcher (%-15s): ---+\n" VERBOSE_PREFIX_3 "|ignore     |exists     |can match  |match more|\n" VERBOSE_PREFIX_3 "|%3s        |%3s        |%3s        |%3s       |\n" VERBOSE_PREFIX_3 "+----------------------------------------------+\n", channel->dialedNumber, ignore_pat ? "yes" : "no", ext_exist ? "yes" : "no", ext_canmatch ? "yes" : "no", ext_matchmore ? "yes" : "no");

	if (ignore_pat) {
		return SCCP_EXTENSION_NOTEXISTS;
	}
	if (ext_exist) {
		if (ext_canmatch && !ext_matchmore) {
			return SCCP_EXTENSION_EXACTMATCH;
		} 
		return SCCP_EXTENSION_MATCHMORE;
	}

	return SCCP_EXTENSION_NOTEXISTS;
}

static PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_request(const char *type, struct ast_format_cap *cap, const struct ast_assigned_ids *assignedids, const struct ast_channel *requestor, const char *dest, int *cause)
{
	sccp_channel_request_status_t requestStatus;
	PBX_CHANNEL_TYPE *result_ast_channel = NULL;
	struct ast_str *codec_buf = ast_str_alloca(64);
	ast_callid callid = 0;

	skinny_codec_t audioCapabilities[SKINNY_MAX_CAPABILITIES];
	skinny_codec_t videoCapabilities[SKINNY_MAX_CAPABILITIES];

	memset(&audioCapabilities, 0, sizeof(audioCapabilities));
	memset(&videoCapabilities, 0, sizeof(videoCapabilities));

	//! \todo parse request
	char *lineName;
	skinny_codec_t codec = SKINNY_CODEC_G711_ULAW_64K;
	sccp_autoanswer_t autoanswer_type = SCCP_AUTOANSWER_NONE;
	uint8_t autoanswer_cause = AST_CAUSE_NOTDEFINED;
	skinny_ringtype_t ringermode = GLOB(ringtype);

	if (!(ast_format_cap_has_type(cap, AST_MEDIA_TYPE_AUDIO))) {
		ast_log(LOG_NOTICE, "Asked to get a channel with an unsupported format '%s'\n", ast_format_cap_get_names(cap, &codec_buf));
		
		/*! \todo transcode or return NULL ? */
		// return NULL;
	}

	*cause = AST_CAUSE_NOTDEFINED;
	if (!type) {
		pbx_log(LOG_NOTICE, "Attempt to call with unspecified type of channel\n");
		*cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
		return NULL;
	}

	if (!dest) {
		pbx_log(LOG_NOTICE, "Attempt to call SCCP/ failed\n");
		*cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
		return NULL;
	}
	/* we leave the data unchanged */
	lineName = pbx_strdupa((const char *) dest);

	/* parsing options string */
	char *options = NULL;
	if ((options = strchr(lineName, '/'))) {
		*options = '\0';
		options++;
	}
	
	// sccp_log(DEBUGCAT_CHANNEL) (VERBOSE_PREFIX_3 "SCCP: Asterisk asked us to create a channel with type=%s, format=" UI64FMT ", lineName=%s, options=%s\n", type, (uint64_t) ast_format_compatibility_codec2bitfield(cap), lineName, (options) ? options : "");
	sccp_log(DEBUGCAT_CHANNEL) (VERBOSE_PREFIX_3 "SCCP: Asterisk asked us to create a channel with type=%s, format=%s, lineName=%s, options=%s\n", type, ast_format_cap_get_names(cap, &codec_buf), lineName, (options) ? options : "");
	if (requestor) {							/* get ringer mode from ALERT_INFO */
		sccp_parse_alertinfo((PBX_CHANNEL_TYPE *)requestor, &ringermode);
	}
	sccp_parse_dial_options(options, &autoanswer_type, &autoanswer_cause, &ringermode);
	if (autoanswer_cause) {
		*cause = autoanswer_cause;
	}

	/** getting remote capabilities */
	char cap_buf[512];

	/* audio capabilities */
	if (requestor) {
		AUTO_RELEASE(sccp_channel_t, remoteSccpChannel , get_sccp_channel_from_pbx_channel(requestor));
		if (remoteSccpChannel) {
			uint8_t x, y, z;

			z = 0;
			/* shrink audioCapabilities to remote preferred/capable format */
			for (x = 0; x < SKINNY_MAX_CAPABILITIES && remoteSccpChannel->preferences.audio[x] != 0; x++) {
				for (y = 0; y < SKINNY_MAX_CAPABILITIES && remoteSccpChannel->capabilities.audio[y] != 0; y++) {
					if (remoteSccpChannel->preferences.audio[x] == remoteSccpChannel->capabilities.audio[y]) {
						audioCapabilities[z++] = remoteSccpChannel->preferences.audio[x];
						break;
					}
				}
			}
		} else {
			if (!sccp_asterisk115_getSkinnyFormatMultiple(ast_channel_nativeformats(requestor), audioCapabilities, ARRAY_LEN(audioCapabilities))) {
				pbx_log(LOG_NOTICE, "SCCP: remote native format is not compatible with any skinny format. Transcoding required\n");
				audioCapabilities[0] = SKINNY_CODEC_WIDEBAND_256K;
			}
		}

		/* video capabilities */
		sccp_asterisk115_getSkinnyFormatMultiple(ast_channel_nativeformats(requestor), videoCapabilities, ARRAY_LEN(videoCapabilities));	//replace AUDIO_MASK with AST_FORMAT_TYPE_AUDIO check
	}

	sccp_codec_multiple2str(cap_buf, sizeof(cap_buf) - 1, audioCapabilities, ARRAY_LEN(audioCapabilities));
	// sccp_log(DEBUGCAT_CODEC) (VERBOSE_PREFIX_4 "remote audio caps: %s\n", cap_buf);

	sccp_codec_multiple2str(cap_buf, sizeof(cap_buf) - 1, videoCapabilities, ARRAY_LEN(videoCapabilities));
	// sccp_log(DEBUGCAT_CODEC) (VERBOSE_PREFIX_4 "remote video caps: %s\n", cap_buf);
	/** done */

	/** get requested format */
	//codec = pbx_codec2skinny_codec(ast_format_compatibility_format2bitfield(format));
	codec = sccp_asterisk115_getSkinnyFormatSingle(cap);
	sccp_log(DEBUGCAT_CODEC) (VERBOSE_PREFIX_4 "SCCP: requestedCodec in Skinny Format: %d\n", codec);

	int callid_created = ast_callid_threadstorage_auto(&callid);

	AUTO_RELEASE(sccp_channel_t, channel , NULL);
	requestStatus = sccp_requestChannel(lineName, codec, audioCapabilities, ARRAY_LEN(audioCapabilities), autoanswer_type, autoanswer_cause, ringermode, &channel);
	switch (requestStatus) {
		case SCCP_REQUEST_STATUS_SUCCESS:								// everything is fine
			break;
		case SCCP_REQUEST_STATUS_LINEUNKNOWN:
			sccp_log(DEBUGCAT_CORE) (VERBOSE_PREFIX_4 "SCCP: sccp_requestChannel returned Line %s Unknown -> Not Successfull\n", lineName);
			*cause = AST_CAUSE_DESTINATION_OUT_OF_ORDER;
			goto EXITFUNC;
		case SCCP_REQUEST_STATUS_LINEUNAVAIL:
			sccp_log(DEBUGCAT_CORE) (VERBOSE_PREFIX_4 "SCCP: sccp_requestChannel returned Line %s not currently registered -> Try again later\n", lineName);
			*cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
			goto EXITFUNC;
		case SCCP_REQUEST_STATUS_ERROR:
			pbx_log(LOG_ERROR, "SCCP: sccp_requestChannel returned Status Error for lineName: %s\n", lineName);
			*cause = AST_CAUSE_UNALLOCATED;
			goto EXITFUNC;
		default:
			pbx_log(LOG_ERROR, "SCCP: sccp_requestChannel returned Status Error for lineName: %s\n", lineName);
			*cause = AST_CAUSE_UNALLOCATED;
			goto EXITFUNC;
	}
	
	if (!sccp_pbx_channel_allocate(channel, assignedids, requestor)) {
		//! \todo handle error in more detail, cleanup sccp channel
		pbx_log(LOG_WARNING, "SCCP: Unable to allocate channel\n");
		*cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
		goto EXITFUNC;
	}

	/* set initial connected line information, to be exchange with remove party during first CONNECTED_LINE update */
	ast_set_callerid(channel->owner, channel->line->cid_num, channel->line->cid_name, channel->line->cid_num);
	struct ast_party_connected_line connected;
	ast_party_connected_line_set_init(&connected, ast_channel_connected(channel->owner));
	connected.id.number.valid = 1;
	connected.id.number.str = (char *)channel->line->cid_num;
	connected.id.number.presentation = AST_PRES_ALLOWED_NETWORK_NUMBER;
	connected.id.name.valid = 1;
	connected.id.name.str = (char *)channel->line->cid_name;
	connected.id.name.presentation = AST_PRES_ALLOWED_NETWORK_NUMBER;
	connected.source = AST_CONNECTED_LINE_UPDATE_SOURCE_UNKNOWN;
	ast_channel_set_connected_line(channel->owner, &connected, NULL);
	/* end */

	if (requestor) {
		/* set calling party */
		sccp_callinfo_t *ci = sccp_channel_getCallInfo(channel);
		iCallInfo.Setter(ci, 
				SCCP_CALLINFO_CALLINGPARTY_NAME, ast_channel_caller((PBX_CHANNEL_TYPE *) requestor)->id.name.str,
				SCCP_CALLINFO_CALLINGPARTY_NUMBER, ast_channel_caller((PBX_CHANNEL_TYPE *) requestor)->id.number.str,
				SCCP_CALLINFO_ORIG_CALLEDPARTY_NAME, ast_channel_redirecting((PBX_CHANNEL_TYPE *) requestor)->orig.name.str,
				SCCP_CALLINFO_ORIG_CALLEDPARTY_NUMBER, ast_channel_redirecting((PBX_CHANNEL_TYPE *) requestor)->orig.number.str,
				SCCP_CALLINFO_KEY_SENTINEL);
	}

	/** workaround for asterisk console log flooded
	 channel.c:5080 ast_write: Codec mismatch on channel SCCP/xxx-0000002d setting write format to g722 from unknown native formats (nothing)
	*/
	if (!channel->capabilities.audio[0]) {
		skinny_codec_t codecs[] = { SKINNY_CODEC_WIDEBAND_256K };
		sccp_wrapper_asterisk115_setNativeAudioFormats(channel, codecs, 1);
		sccp_wrapper_asterisk115_setReadFormat(channel, SKINNY_CODEC_WIDEBAND_256K);
		sccp_wrapper_asterisk115_setWriteFormat(channel, SKINNY_CODEC_WIDEBAND_256K);
	}
	
	/* get remote codecs from channel driver */
	//ast_rtp_instance_get_codecs(c->rtp.adio.rtp);
	//ast_rtp_instance_get_codecs(c->rtp.video.instance);
	/** done */

EXITFUNC:
	if (channel) {
		result_ast_channel = channel->owner;
		if (callid) {
			ast_channel_callid_set(result_ast_channel, callid);
		}
		channel->pbx_callid_created = callid_created;
	}
	return result_ast_channel;
}

static int sccp_wrapper_asterisk115_call(PBX_CHANNEL_TYPE * ast, const char *dest, int timeout)
{
	struct varshead *headp;
	struct ast_var_t *current;

	int res = 0;

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Asterisk request to call %s (dest:%s, timeout: %d)\n", pbx_channel_name(ast), dest, timeout);

	if (!sccp_strlen_zero(pbx_channel_call_forward(ast))) {
		iPbx.queue_control(ast, -1);									/* Prod Channel if in the middle of a call_forward instead of proceed */
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Forwarding Call to '%s'\n", pbx_channel_call_forward(ast));
		return 0;
	}

	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));
	if (!c) {
		pbx_log(LOG_WARNING, "SCCP: Asterisk request to call %s on channel: %s, but we don't have this channel!\n", dest, pbx_channel_name(ast));
		return -1;
	}

	/* Check whether there is MaxCallBR variables */
	headp = ast_channel_varshead(ast);
	//ast_log(LOG_NOTICE, "SCCP: search for varibles!\n");

	AST_LIST_TRAVERSE(headp, current, entries) {
		//ast_log(LOG_NOTICE, "var: name: %s, value: %s\n", ast_var_name(current), ast_var_value(current));
		if (!strcasecmp(ast_var_name(current), "__MaxCallBR")) {
			sccp_asterisk_pbx_fktChannelWrite(ast, "CHANNEL", "MaxCallBR", ast_var_value(current));
		} else if (!strcasecmp(ast_var_name(current), "MaxCallBR")) {
			sccp_asterisk_pbx_fktChannelWrite(ast, "CHANNEL", "MaxCallBR", ast_var_value(current));
#if CS_SCCP_VIDEO
		} else if (!strcasecmp(ast_var_name(current), "SCCP_VIDEO_MODE")) {
			sccp_channel_setVideoMode(c, ast_var_value(current));
#endif
		}
	}

	res = sccp_pbx_call(c, (char *) dest, timeout);
	return res;
}

static int sccp_wrapper_asterisk115_answer(PBX_CHANNEL_TYPE * chan)
{
	//! \todo change this handling and split pbx and sccp handling -MC
	int res = -1;

	AUTO_RELEASE(sccp_channel_t, channel , get_sccp_channel_from_pbx_channel(chan));
	if (channel) {
		if (!channel->pbx_callid_created && !ast_channel_callid(chan)) {
			ast_callid_threadassoc_add(ast_channel_callid(chan));
		}
		res = sccp_pbx_answered(channel);
	}
	return res;
}

/**
 *
 * \todo update remote capabilities after fixup
 */
static int sccp_wrapper_asterisk115_fixup(PBX_CHANNEL_TYPE * oldchan, PBX_CHANNEL_TYPE * newchan)
{
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: we got a fixup request for %s and %s\n", pbx_channel_name(oldchan), pbx_channel_name(newchan));
	int res = 0;

	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(newchan));
	if (c) {
		if (c->owner != oldchan) {
			ast_log(LOG_WARNING, "old channel wasn't %p but was %p\n", oldchan, c->owner);
			res = -1;
		} else {
			/* during a masquerade, fixup gets called twice, The Zombie channel name will have been changed to include '<ZOMBI>' */
			/* using test_flag for ZOMBIE cannot be used, as it is only set after the fixup call */
			if (!strstr(pbx_channel_name(newchan), "<ZOMBIE>")) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: set c->hangupRequest = requestQueueHangup\n", c->designator);

				// set channel requestHangup to use ast_queue_hangup (as it is now part of a __ast_pbx_run, after masquerade completes)
				c->hangupRequest = sccp_wrapper_asterisk_requestQueueHangup;
				if (!sccp_strlen_zero(c->line->language)) {
					ast_channel_language_set(newchan, c->line->language);
				}
			} else {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: set c->hangupRequest = requestHangup\n", c->designator);
				// set channel requestHangup to use ast_hangup (as it will not be part of __ast_pbx_run anymore, upon returning from masquerade)
				c->hangupRequest = sccp_wrapper_asterisk_requestHangup;
			}
			// c->owner = ast_channel_ref(newchan);
			// ast_channel_unref(oldchan);
			sccp_wrapper_asterisk115_setOwner(c, newchan);
			//! \todo force update of rtp peer for directrtp
			// sccp_wrapper_asterisk115_update_rtp_peer(newchan, NULL, NULL, 0, 0, 0);

			//! \todo update remote capabilities after fixup

		}
	} else {
		pbx_log(LOG_WARNING, "sccp_pbx_fixup(old: %s(%p), new: %s(%p)). no SCCP channel to fix\n", pbx_channel_name(oldchan), (void *) oldchan, pbx_channel_name(newchan), (void *) newchan);
		res = -1;
	}
	return res;
}

#if 0
#ifdef CS_AST_RTP_INSTANCE_BRIDGE
static enum ast_bridge_result sccp_wrapper_asterisk115_rtpBridge(PBX_CHANNEL_TYPE * c0, PBX_CHANNEL_TYPE * c1, int flags, PBX_FRAME_TYPE ** fo, PBX_CHANNEL_TYPE ** rc, int timeoutms)
{
	enum ast_bridge_result res;
	int new_flags = flags;

	/* \note temporarily marked out until we figure out how to get directrtp back on track - DdG */
	AUTO_RELEASE(sccp_channel_t, sc0 , get_sccp_channel_from_pbx_channel(c0));
	AUTO_RELEASE(sccp_channel_t, sc1 , get_sccp_channel_from_pbx_channel(c1));

	if ((sc0 && sc1)) {
		// Switch off DTMF between SCCP phones
		new_flags &= !AST_BRIDGE_DTMF_CHANNEL_0;
		new_flags &= !AST_BRIDGE_DTMF_CHANNEL_1;
		if (GLOB(directrtp)) {
			ast_channel_defer_dtmf(c0);
			ast_channel_defer_dtmf(c1);
		} else {
			AUTO_RELEASE(sccp_device_t, d0 , sccp_channel_getDevice(sc0));
			AUTO_RELEASE(sccp_device_t, d1 , sccp_channel_getDevice(sc1));
			if ((d0 && d1) && ((d0->directrtp && d1->directrtp)) {
				ast_channel_defer_dtmf(c0);
				ast_channel_defer_dtmf(c1);
			}
		}
		sc0->peerIsSCCP = TRUE;
		sc1->peerIsSCCP = TRUE;
		// SCCP Key handle direction to asterisk is still to be implemented here
		// sccp_pbx_senddigit
	} else {
		// Switch on DTMF between differing channels
		ast_channel_undefer_dtmf(c0);
		ast_channel_undefer_dtmf(c1);
	}
	//res = ast_rtp_bridge(c0, c1, new_flags, fo, rc, timeoutms);
	res = ast_rtp_instance_bridge(c0, c1, new_flags, fo, rc, timeoutms);
	switch (res) {
		case AST_BRIDGE_COMPLETE:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "SCCP: Bridge chan %s and chan %s: Complete\n", ast_channel_name(c0), ast_channel_name(c1));
			break;
		case AST_BRIDGE_FAILED:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "SCCP: Bridge chan %s and chan %s: Failed\n", ast_channel_name(c0), ast_channel_name(c1));
			break;
		case AST_BRIDGE_FAILED_NOWARN:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "SCCP: Bridge chan %s and chan %s: Failed NoWarn\n", ast_channel_name(c0), ast_channel_name(c1));
			break;
		case AST_BRIDGE_RETRY:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "SCCP: Bridge chan %s and chan %s: Failed Retry\n", ast_channel_name(c0), ast_channel_name(c1));
			break;
	}
	/*! \todo Implement callback function queue upon completion */
	return res;
}
#endif
#endif

static enum ast_rtp_glue_result sccp_wrapper_asterisk115_get_rtp_info(PBX_CHANNEL_TYPE * ast, PBX_RTP_TYPE ** rtp)
{
	//AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));				// not following the refcount rules... channel is already retained
	sccp_channel_t *c = NULL;
	sccp_rtp_info_t rtpInfo = SCCP_RTP_INFO_NORTP;
	struct sccp_rtp *audioRTP = NULL;
	enum ast_rtp_glue_result res = AST_RTP_GLUE_RESULT_REMOTE;

	if (!(c = CS_AST_CHANNEL_PVT(ast))) {
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "SCCP: (asterisk115_get_rtp_info) NO PVT\n");
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	if (pbx_channel_state(ast) != AST_STATE_UP) {
		sccp_log((DEBUGCAT_CHANNEL | DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_rtp_info) Asterisk requested EarlyRTP peer for channel %s\n", c->currentDeviceId, pbx_channel_name(ast));
	} else {
		sccp_log((DEBUGCAT_CHANNEL | DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_rtp_info) Asterisk requested RTP peer for channel %s\n", c->currentDeviceId, pbx_channel_name(ast));
	}

	rtpInfo = sccp_rtp_getAudioPeerInfo(c, &audioRTP);
	if (rtpInfo == SCCP_RTP_INFO_NORTP) {
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	*rtp = audioRTP->instance;
	if (!*rtp) {
		return AST_RTP_GLUE_RESULT_FORBID;
	}
	// struct ast_sockaddr ast_sockaddr_tmp;
	// ast_rtp_instance_get_remote_address(*rtp, &ast_sockaddr_tmp);
	// sccp_log((DEBUGCAT_RTP | DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: (asterisk115_get_rtp_info) remote address:'%s:%d'\n", c->currentDeviceId, ast_sockaddr_stringify_host(&ast_sockaddr_tmp), ast_sockaddr_port(&ast_sockaddr_tmp));

	ao2_ref(*rtp, +1);
	if (ast_test_flag(GLOB(global_jbconf), AST_JB_FORCED)) {
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_rtp_info) JitterBuffer is Forced. AST_RTP_GET_FAILED\n", c->currentDeviceId);
		return AST_RTP_GLUE_RESULT_LOCAL;
	}

	if (!(rtpInfo & SCCP_RTP_INFO_ALLOW_DIRECTRTP)) {
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_rtp_info) Direct RTP disabled ->  Using AST_RTP_TRY_PARTIAL for channel %s\n", c->currentDeviceId, pbx_channel_name(ast));
		return AST_RTP_GLUE_RESULT_LOCAL;
	}

	sccp_log((DEBUGCAT_RTP | DEBUGCAT_HIGH)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_rtp_info) Channel %s Returning res: %s\n", c->currentDeviceId, pbx_channel_name(ast), ((res == 2) ? "indirect-rtp" : ((res == 1) ? "direct-rtp" : "forbid")));
	return res;
}

static enum ast_rtp_glue_result sccp_wrapper_asterisk115_get_vrtp_info(PBX_CHANNEL_TYPE * ast, PBX_RTP_TYPE ** rtp)
{
	//AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));				// not following the refcount rules... channel is already retained
	sccp_channel_t *c = NULL;
	sccp_rtp_info_t rtpInfo = SCCP_RTP_INFO_NORTP;
	struct sccp_rtp *videoRTP = NULL;
	enum ast_rtp_glue_result res = AST_RTP_GLUE_RESULT_REMOTE;

	if (!(c = CS_AST_CHANNEL_PVT(ast))) {
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "SCCP: (asterisk115_get_vrtp_info) NO PVT\n");
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	if (pbx_channel_state(ast) != AST_STATE_UP) {
		sccp_log((DEBUGCAT_CHANNEL | DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_vrtp_info) Asterisk requested EarlyRTP peer for channel %s\n", c->currentDeviceId, pbx_channel_name(ast));
	} else {
		sccp_log((DEBUGCAT_CHANNEL | DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_vrtp_info) Asterisk requested RTP peer for channel %s\n", c->currentDeviceId, pbx_channel_name(ast));
	}

#ifdef CS_SCCP_VIDEO
	rtpInfo = sccp_rtp_getVideoPeerInfo(c, &videoRTP);
#endif
	if (rtpInfo == SCCP_RTP_INFO_NORTP) {
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	*rtp = videoRTP->instance;
	if (!*rtp) {
		return AST_RTP_GLUE_RESULT_FORBID;
	}
	// struct ast_sockaddr ast_sockaddr_tmp;
	// ast_rtp_instance_get_remote_address(*rtp, &ast_sockaddr_tmp);
	// sccp_log((DEBUGCAT_RTP | DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: (asterisk115_get_rtp_info) remote address:'%s:%d'\n", c->currentDeviceId, ast_sockaddr_stringify_host(&ast_sockaddr_tmp), ast_sockaddr_port(&ast_sockaddr_tmp));
#ifdef HAVE_PBX_RTP_ENGINE_H
	ao2_ref(*rtp, +1);
#endif
	if (ast_test_flag(GLOB(global_jbconf), AST_JB_FORCED)) {
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_vrtp_info) JitterBuffer is Forced. AST_RTP_GET_FAILED\n", c->currentDeviceId);
		return AST_RTP_GLUE_RESULT_FORBID;
	}

	if (!(rtpInfo & SCCP_RTP_INFO_ALLOW_DIRECTRTP)) {
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_vrtp_info) Direct RTP disabled ->  Using AST_RTP_TRY_PARTIAL for channel %s\n", c->currentDeviceId, pbx_channel_name(ast));
		return AST_RTP_GLUE_RESULT_LOCAL;
	}

	sccp_log((DEBUGCAT_RTP | DEBUGCAT_HIGH)) (VERBOSE_PREFIX_1 "%s: (asterisk115_get_vrtp_info) Channel %s Returning res: %s\n", c->currentDeviceId, pbx_channel_name(ast), ((res == 2) ? "indirect-rtp" : ((res == 1) ? "direct-rtp" : "forbid")));
	return res;
}

static int sccp_wrapper_asterisk115_update_rtp_peer(PBX_CHANNEL_TYPE * ast, PBX_RTP_TYPE * rtp, PBX_RTP_TYPE * vrtp, PBX_RTP_TYPE * trtp, const struct ast_format_cap *codecs, int nat_active)
{
	//AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));				// not following the refcount rules... channel is already retained
	sccp_channel_t *c = NULL;
	int result = 0;
	do {
		struct ast_str *codec_buf = ast_str_alloca(64);

		if (!(c = CS_AST_CHANNEL_PVT(ast))) {
			sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "SCCP: (asterisk115_update_rtp_peer) NO PVT\n");
			result = -1;
			break;
		}

		if (!codecs) {
			sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_update_rtp_peer) NO Codecs\n", c->currentDeviceId);
			result = -1;
			break;
		}
		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_2 "%s: (asterisk115_update_rtp_peer) stage: %s, remote codecs capabilty: %s (%lu), nat_active: %d\n", c->currentDeviceId, S_COR(AST_STATE_UP == pbx_channel_state(ast), "RTP", "EarlyRTP"), ast_format_cap_get_names((struct ast_format_cap *) codecs, &codec_buf), (long unsigned int) codecs, nat_active);

		if (!c->line) {
			sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_update_rtp_peer) NO LINE\n", c->currentDeviceId);
			result = -1;
			break;
		}
		if (c->state == SCCP_CHANNELSTATE_HOLD) {
			sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_update_rtp_peer) On Hold -> No Update\n", c->currentDeviceId);
			result = 0;
			break;
		}
		AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));
		if (!d) {
			sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_update_rtp_peer) NO DEVICE\n", c->currentDeviceId);
			result = -1;
			break;
		}

		if (!rtp && !vrtp && !trtp) {
			sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_update_rtp_peer) RTP not ready\n", c->currentDeviceId);
			result = 0;
			break;
		}

		PBX_RTP_TYPE *instance = { 0, };
		struct sockaddr_storage sas = { 0, };
		//struct sockaddr_in sin = { 0, };
		struct ast_sockaddr sin_tmp;
		boolean_t directmedia = FALSE;

		if (rtp) {											// generalize input
			instance = rtp;
		} else if (vrtp) {
			instance = vrtp;
#ifdef CS_SCCP_VIDEO			
			/* video requested by remote side, let's see if we support video */ 
 			/* should be moved to sccp_rtp.c */
/*
			if (ast_format_cap_has_type(codecs, AST_MEDIA_TYPE_VIDEO) && sccp_device_isVideoSupported(d) && c->videomode == SCCP_VIDEO_MODE_AUTO) {
				if (!c->rtp.video.instance && !sccp_rtp_createVideoServer(c)) {
					sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: can not start vrtp\n", DEV_ID_LOG(d));
				} else {
					if (!c->rtp.video.mediaTransmissionState) {
						sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_3 "%s: video rtp started\n", DEV_ID_LOG(d));
						sccp_channel_startMultiMediaTransmission(c);
					}
				}
			}
*/
#endif			
		} else {
			instance = trtp;
		}

		if (d->directrtp && d->nat < SCCP_NAT_ON && !nat_active && !c->conference) {			// asume directrtp
			ast_rtp_instance_get_remote_address(instance, &sin_tmp);
			memcpy(&sas, &sin_tmp, sizeof(struct sockaddr_storage));
			//ast_sockaddr_to_sin(&sin_tmp, &sin);
			if (d->nat == SCCP_NAT_OFF) {								// forced nat off to circumvent autodetection + direcrtp, requires checking both phone_ip and external session ip address against devices permit/deny
				struct ast_sockaddr sin_local;
				struct sockaddr_storage localsas = { 0, };
				ast_rtp_instance_get_local_address(instance, &sin_local);
				memcpy(&localsas, &sin_local, sizeof(struct sockaddr_storage));
				if (sccp_apply_ha(d->ha, &sas) == AST_SENSE_ALLOW && sccp_apply_ha(d->ha, &localsas) == AST_SENSE_ALLOW) {
					directmedia = TRUE;
				}
			} else if (sccp_apply_ha(d->ha, &sas) == AST_SENSE_ALLOW) {					// check remote sin against local device acl (to match netmask)
				directmedia = TRUE;
				// ast_channel_defer_dtmf(ast);
			}
		}
		if (!directmedia) {										// fallback to indirectrtp
			ast_rtp_instance_get_local_address(instance, &sin_tmp);
			memcpy(&sas, &sin_tmp, sizeof(struct sockaddr_storage));
			sccp_session_getOurIP(d->session, &sas, sccp_netsock_is_IPv4(&sas) ? AF_INET : AF_INET6);
		}

		sccp_log((DEBUGCAT_RTP)) (VERBOSE_PREFIX_1 "%s: (asterisk115_update_rtp_peer) new remote rtp ip = '%s'\n (d->directrtp: %s && !d->nat: %s && !remote->nat_active: %s && d->acl_allow: %s) => directmedia=%s\n", c->currentDeviceId, sccp_netsock_stringify(&sas), S_COR(d->directrtp, "yes", "no"),
					  sccp_nat2str(d->nat),
					  S_COR(!nat_active, "yes", "no"), S_COR(directmedia, "yes", "no"), S_COR(directmedia, "yes", "no")
		    );

		if (rtp) {											// send peer info to phone
			sccp_rtp_set_peer(c, &c->rtp.audio, &sas);
			c->rtp.audio.directMedia = directmedia;
		} else if (vrtp) {
			sccp_rtp_set_peer(c, &c->rtp.video, &sas);
			c->rtp.video.directMedia = directmedia;
		} else {
			//sccp_rtp_set_peer(c, &c->rtp.text, &sas);
			//c->rtp.text.directMedia = directmedia;
		}
	} while (0);

	/* Need a return here to break the bridge */
	sccp_log(DEBUGCAT_RTP) (VERBOSE_PREFIX_3 "%s: (asterisk115_update_rtp_peer) Result: %d\n", c->currentDeviceId, result);
	return result;
}

static void sccp_wrapper_asterisk115_getCodec(PBX_CHANNEL_TYPE * ast, struct ast_format_cap *result)
{
	uint8_t i;
	AUTO_RELEASE(sccp_channel_t, channel , get_sccp_channel_from_pbx_channel(ast));

	if (!channel) {
		sccp_log((DEBUGCAT_RTP | DEBUGCAT_CODEC)) (VERBOSE_PREFIX_1 "SCCP: (getCodec) NO PVT\n");
		return;
	}

	ast_debug(10, "asterisk requests format for channel %s, readFormat: %s(%d)\n", pbx_channel_name(ast), codec2str(channel->rtp.audio.readFormat), channel->rtp.audio.readFormat);
	for (i = 0; i < ARRAY_LEN(channel->preferences.audio); i++) {
		struct ast_format *ast_format = sccp_asterisk115_skinny2ast_format(channel->preferences.audio[i]);
		if (ast_format != ast_format_none) {
			unsigned int framing = ast_format_get_default_ms(ast_format);
			ast_format_cap_append(result, ast_format, framing);
		}
	}
	for (i = 0; i < ARRAY_LEN(channel->preferences.video); i++) {
		struct ast_format *ast_format = sccp_asterisk115_skinny2ast_format(channel->preferences.video[i]);
		if (ast_format != ast_format_none) {
			unsigned int framing = ast_format_get_default_ms(ast_format);
			ast_format_cap_append(result, ast_format, framing);
		}
	}

	return;
}

/*
 * \brief get callerid_name from pbx
 * \param sccp_channle Asterisk Channel
 * \param cid name result
 * \return parse result
 */
static int sccp_wrapper_asterisk115_callerid_name(PBX_CHANNEL_TYPE *pbx_chan, char **cid_name)
{
	if (pbx_chan && ast_channel_caller(pbx_chan)->id.name.str && strlen(ast_channel_caller(pbx_chan)->id.name.str) > 0) {
		*cid_name = pbx_strdup(ast_channel_caller(pbx_chan)->id.name.str);
		return 1;
	}

	return 0;
}

/*
 * \brief get callerid_name from pbx
 * \param ast_chan Asterisk Channel
 * \return char * with the caller number
 */
static int sccp_wrapper_asterisk115_callerid_number(PBX_CHANNEL_TYPE *pbx_chan, char **cid_number)
{
	if (pbx_chan && ast_channel_caller(pbx_chan)->id.number.str && strlen(ast_channel_caller(pbx_chan)->id.number.str) > 0) {
		*cid_number = pbx_strdup(ast_channel_caller(pbx_chan)->id.number.str);
		return 1;
	}

	return 0;
}

/*
 * \brief get callerid_ton from pbx
 * \param ast_chan Asterisk Channel
 * \return char * with the caller number
 */
static int sccp_wrapper_asterisk115_callerid_ton(PBX_CHANNEL_TYPE *pbx_chan, int *cid_ton)
{
	if (pbx_chan && ast_channel_caller(pbx_chan)->id.number.valid) {
		*cid_ton = ast_channel_caller(pbx_chan)->ani.number.plan;
		return *cid_ton;
	}
	return 0;
}

/*
 * \brief get callerid_ani from pbx
 * \param ast_chan Asterisk Channel
 * \return char * with the caller number
 */
static int sccp_wrapper_asterisk115_callerid_ani(PBX_CHANNEL_TYPE *pbx_chan, char **cid_ani)
{
	if (pbx_chan && ast_channel_caller(pbx_chan)->ani.number.valid && ast_channel_caller(pbx_chan)->ani.number.str && strlen(ast_channel_caller(pbx_chan)->ani.number.str) > 0) {
		*cid_ani = pbx_strdup(ast_channel_caller(pbx_chan)->ani.number.str);
		return 1;
	}

	return 0;
}

/*
 * \brief get callerid_dnid from pbx
 * \param ast_chan Asterisk Channel
 * \return char * with the caller number
 */
static int sccp_wrapper_asterisk115_callerid_subaddr(PBX_CHANNEL_TYPE *pbx_chan, char **cid_subaddr)
{
	if (pbx_chan && ast_channel_caller(pbx_chan)->id.subaddress.valid && ast_channel_caller(pbx_chan)->id.subaddress.str && strlen(ast_channel_caller(pbx_chan)->id.subaddress.str) > 0) {
		*cid_subaddr = pbx_strdup(ast_channel_caller(pbx_chan)->id.subaddress.str);
		return 1;
	}

	return 0;
}

/*
 * \brief get callerid_dnid from pbx (Destination ID)
 * \param ast_chan Asterisk Channel
 * \return char * with the caller number
 */
static int sccp_wrapper_asterisk115_callerid_dnid(PBX_CHANNEL_TYPE *pbx_chan, char **cid_dnid)
{
	if (pbx_chan && ast_channel_dialed(pbx_chan)->number.str && strlen(ast_channel_dialed(pbx_chan)->number.str) > 0) {
		*cid_dnid = pbx_strdup(ast_channel_dialed(pbx_chan)->number.str);
		return 1;
	}

	return 0;
}

/*
 * \brief get callerid_rdnis from pbx
 * \param ast_chan Asterisk Channel
 * \return char * with the caller number
 */
static int sccp_wrapper_asterisk115_callerid_rdnis(PBX_CHANNEL_TYPE *pbx_chan, char **cid_rdnis)
{
	if (pbx_chan && ast_channel_redirecting(pbx_chan)->from.number.valid && ast_channel_redirecting(pbx_chan)->from.number.str && strlen(ast_channel_redirecting(pbx_chan)->from.number.str) > 0) {
		*cid_rdnis = pbx_strdup(ast_channel_redirecting(pbx_chan)->from.number.str);
		return 1;
	}

	return 0;
}

/*
 * \brief get callerid_presence from pbx
 * \param ast_chan Asterisk Channel
 * \return char * with the caller number
 */
static int sccp_wrapper_asterisk115_callerid_presentation(PBX_CHANNEL_TYPE *pbx_chan)
{
	if (pbx_chan && (ast_party_id_presentation(&ast_channel_caller(pbx_chan)->id) & AST_PRES_RESTRICTION) == AST_PRES_ALLOWED) {
		return CALLERID_PRESENTATION_ALLOWED;
	}
	return CALLERID_PRESENTATION_FORBIDDEN;
}

static boolean_t sccp_wrapper_asterisk115_createRtpInstance(constDevicePtr d, constChannelPtr c, sccp_rtp_t *rtp)
{
	struct ast_sockaddr sock = { {0,} };
	uint32_t tos = 0, cos = 0;
	
	if (!c || !d) {
		return FALSE;
	}
	memcpy(&sock.ss, &GLOB(bindaddr), sizeof(struct sockaddr_storage));
	if (GLOB(bindaddr).ss_family == AF_INET6) {
		sock.ss.ss_family = AF_INET6;
		sock.len = sizeof(struct sockaddr_in6);
	} else {
		sock.ss.ss_family = AF_INET;
		sock.len = sizeof(struct sockaddr_in);
	}

	if ((rtp->instance = ast_rtp_instance_new("asterisk", sched, &sock, NULL))) {
		struct ast_sockaddr instance_addr = { {0,} };
		ast_rtp_instance_get_local_address(rtp->instance, &instance_addr);
		sccp_log(DEBUGCAT_RTP) (VERBOSE_PREFIX_3 "%s: rtp server instance created at %s\n", c->designator, ast_sockaddr_stringify(&instance_addr));
	} else {
		return FALSE;
	}

	/* rest below should be moved out of here (refactoring required) */
	PBX_RTP_TYPE *instance = rtp->instance;
	//char *rtp_map_filter = NULL;
	//skinny_payload_type_t codec_type;
	int fd_offset = 0;
	switch(rtp->type) {
		case SCCP_RTP_AUDIO:
			tos = d->audio_tos;
			cos = d->audio_cos;
			//rtp_map_filter = "audio";
			//codec_type = SKINNY_CODEC_TYPE_AUDIO;
			break;
			
#if CS_SCCP_VIDEO
		case SCCP_RTP_VIDEO:
			tos = d->video_tos;
			cos = d->video_cos;
			//rtp_map_filter = "video";
			//codec_type = SKINNY_CODEC_TYPE_VIDEO;
			fd_offset = 2;
			break;
#endif			
		default:
			pbx_log(LOG_ERROR, "%s: (wrapper_create_rtp) unknown/unhandled rtp type, returning instance for now\n", c->designator);
			return TRUE;
	}

	if (c->owner) {
		ast_channel_stage_snapshot(c->owner);
		ast_channel_set_fd(c->owner, fd_offset, ast_rtp_instance_fd(instance, 0));		// RTP
		ast_channel_set_fd(c->owner, fd_offset + 1, ast_rtp_instance_fd(instance, 1));		// RTCP
	}
	ast_rtp_instance_set_prop(instance, AST_RTP_PROPERTY_RTCP, 1);
	if (rtp->type == SCCP_RTP_AUDIO) {
		ast_rtp_instance_set_prop(instance, AST_RTP_PROPERTY_DTMF, 1);
		if (c->dtmfmode == SCCP_DTMFMODE_SKINNY) {
			ast_rtp_instance_set_prop(instance, AST_RTP_PROPERTY_DTMF_COMPENSATE, 1);
			ast_rtp_instance_dtmf_mode_set(instance, AST_RTP_DTMF_MODE_INBAND);
		}
	}
	ast_rtp_instance_set_qos(instance, tos, cos, "SCCP RTP");

	// Add CISCO DTMF SKINNY payload type
	if (rtp->type == SCCP_RTP_AUDIO && SCCP_DTMFMODE_SKINNY == d->dtmfmode) {
		ast_rtp_codecs_payloads_set_m_type(ast_rtp_instance_get_codecs(c->rtp.audio.instance), c->rtp.audio.instance, 96);
		ast_rtp_codecs_payloads_set_rtpmap_type(ast_rtp_instance_get_codecs(c->rtp.audio.instance), c->rtp.audio.instance, 96, "audio", "telephone-event", 0);
		ast_rtp_codecs_payloads_set_m_type(ast_rtp_instance_get_codecs(c->rtp.audio.instance), c->rtp.audio.instance, 101);
		ast_rtp_codecs_payloads_set_rtpmap_type(ast_rtp_instance_get_codecs(c->rtp.audio.instance), c->rtp.audio.instance, 101, "audio", "telephone-event", 0);
		ast_rtp_codecs_payloads_set_m_type(ast_rtp_instance_get_codecs(c->rtp.audio.instance), c->rtp.audio.instance, 105);
		ast_rtp_codecs_payloads_set_rtpmap_type(ast_rtp_instance_get_codecs(c->rtp.audio.instance), c->rtp.audio.instance, 105, "audio", "cisco-telephone-event", 0);
	}
	ast_rtp_instance_activate(instance);

	if (c->owner) {
		ast_channel_stage_snapshot_done(c->owner);
		// this prevent a warning about unknown codec, when rtp traffic starts */
		ast_queue_frame(c->owner, &ast_null_frame);
	}

	return TRUE;
}

static boolean_t sccp_wrapper_asterisk115_destroyRTP(PBX_RTP_TYPE * rtp)
{
	int res;

	res = ast_rtp_instance_destroy(rtp);
	return (!res) ? TRUE : FALSE;
}

static boolean_t sccp_wrapper_asterisk115_checkHangup(constChannelPtr channel)
{
	int res;

	res = ast_check_hangup(channel->owner);
	return (!res) ? TRUE : FALSE;
}

static boolean_t sccp_wrapper_asterisk115_rtpGetPeer(PBX_RTP_TYPE * rtp, struct sockaddr_storage *address)
{
	struct ast_sockaddr addr;

	ast_rtp_instance_get_remote_address(rtp, &addr);
	memcpy(address, &addr.ss, sizeof(struct sockaddr_storage));
	return TRUE;
}

static boolean_t sccp_wrapper_asterisk115_rtpGetUs(PBX_RTP_TYPE * rtp, struct sockaddr_storage *address)
{
	struct ast_sockaddr addr;

	ast_rtp_instance_get_local_address(rtp, &addr);
	memcpy(address, &addr.ss, sizeof(struct sockaddr_storage));
	return TRUE;
}

static boolean_t sccp_wrapper_asterisk115_getChannelByName(const char *name, PBX_CHANNEL_TYPE ** pbx_channel)
{
	PBX_CHANNEL_TYPE *ast = ast_channel_get_by_name(name);

	if (!ast) {
		return FALSE;
	}
	*pbx_channel = ast;
	return TRUE;
}

//static int sccp_wrapper_asterisk115_rtp_set_peer(const struct sccp_rtp *rtp, const struct sockaddr_storage *new_peer, int nat_active)
static int sccp_wrapper_asterisk115_setPhoneRTPAddress(const struct sccp_rtp *rtp, const struct sockaddr_storage *new_peer, int nat_active)
{
	struct ast_sockaddr ast_sockaddr_dest;
	int res;

	memcpy(&ast_sockaddr_dest.ss, new_peer, sizeof(struct sockaddr_storage));
	ast_sockaddr_dest.len = (ast_sockaddr_dest.ss.ss_family == AF_INET6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
	res = ast_rtp_instance_set_remote_address(rtp->instance, &ast_sockaddr_dest);

	sccp_log((DEBUGCAT_RTP | DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "SCCP: (asterisk115_setPhoneRTPAddress) Update PBX to send RTP/UDP media to '%s' (new remote) (NAT: %s)\n", ast_sockaddr_stringify(&ast_sockaddr_dest), S_COR(nat_active, "yes", "no"));
	if (nat_active) {
		ast_rtp_instance_set_prop(rtp->instance, AST_RTP_PROPERTY_NAT, 1);
	} else {
		ast_rtp_instance_set_prop(rtp->instance, AST_RTP_PROPERTY_NAT, 0);
	}
	return res;
}

static boolean_t sccp_wrapper_asterisk115_setWriteFormat(constChannelPtr channel, skinny_codec_t codec)
{
	if (!channel) {
		return FALSE;
	}

	struct ast_format *ast_format = sccp_asterisk115_skinny2ast_format(codec);
	if (ast_format != ast_format_none) {
		struct ast_format_cap *cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
		if (!cap) {
			ao2_cleanup(cap);
			return FALSE;
		}

		unsigned int framing = ast_format_get_default_ms(ast_format);
		ast_format_cap_append(cap, ast_format, framing);
		ast_set_write_format_from_cap(channel->owner, cap);

		ao2_ref(cap, -1);
		cap = NULL;
	} else {
		return FALSE;
	}
	
	if (NULL != channel->rtp.audio.instance) {
		ast_rtp_instance_set_write_format(channel->rtp.audio.instance, ast_format);
	}
	return TRUE;
}

static boolean_t sccp_wrapper_asterisk115_setReadFormat(constChannelPtr channel, skinny_codec_t codec)
{
	//! \todo possibly needs to be synced to ast108
	if (!channel) {
		return FALSE;
	}

	struct ast_format *ast_format = sccp_asterisk115_skinny2ast_format(codec);
	if (ast_format != ast_format_none) {
		struct ast_format_cap *cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
		if (!cap) {
			ao2_cleanup(cap);
			return FALSE;
		}

		unsigned int framing = ast_format_get_default_ms(ast_format);
		ast_format_cap_append(cap, ast_format, framing);
		ast_set_read_format_from_cap(channel->owner, cap);

		ao2_ref(cap, -1);
		cap = NULL;
	} else {
		return FALSE;
	}

	if (NULL != channel->rtp.audio.instance) {
		ast_rtp_instance_set_read_format(channel->rtp.audio.instance, ast_format);
	}
	return TRUE;
}

static void sccp_wrapper_asterisk115_setCalleridName(PBX_CHANNEL_TYPE *pbx_channel, const char *name)
{
	if (pbx_channel && name) {
		ast_party_name_free(&ast_channel_caller(pbx_channel)->id.name);
		ast_channel_caller(pbx_channel)->id.name.str = pbx_strdup(name);
		ast_channel_caller(pbx_channel)->id.name.valid = 1;
	}
}

static void sccp_wrapper_asterisk115_setCalleridNumber(PBX_CHANNEL_TYPE *pbx_channel, const char *number)
{
	if (pbx_channel && number) {
		ast_party_number_free(&ast_channel_caller(pbx_channel)->id.number);
		ast_channel_caller(pbx_channel)->id.number.str = pbx_strdup(number);
		ast_channel_caller(pbx_channel)->id.number.valid = 1;
	}
}

static void sccp_wrapper_asterisk115_setCalleridAni(PBX_CHANNEL_TYPE *pbx_channel, const char *number)
{
	if (pbx_channel && number) {
		ast_party_number_free(&ast_channel_caller(pbx_channel)->ani.number);
		ast_channel_caller(pbx_channel)->ani.number.str = pbx_strdup(number);
		ast_channel_caller(pbx_channel)->ani.number.valid = 1;
	}
}

static void sccp_wrapper_asterisk115_setRedirectingParty(PBX_CHANNEL_TYPE *pbx_channel, const char *number, const char *name)
{
	if (pbx_channel) {
		if (number) {
			ast_party_number_free(&ast_channel_redirecting(pbx_channel)->from.number);
			ast_channel_redirecting(pbx_channel)->from.number.str = pbx_strdup(number);
			ast_channel_redirecting(pbx_channel)->from.number.valid = 1;
		}

		if (name) {
			ast_party_name_free(&ast_channel_redirecting(pbx_channel)->from.name);
			ast_channel_redirecting(pbx_channel)->from.name.str = pbx_strdup(name);
			ast_channel_redirecting(pbx_channel)->from.name.valid = 1;
		}
	}
}

static void sccp_wrapper_asterisk115_setRedirectedParty(PBX_CHANNEL_TYPE *pbx_channel, const char *number, const char *name)
{
	if (pbx_channel) {
		if (number) {
			ast_party_number_free(&ast_channel_redirecting(pbx_channel)->to.number);
			ast_channel_redirecting(pbx_channel)->to.number.str = pbx_strdup(number);
			ast_channel_redirecting(pbx_channel)->to.number.valid = 1;
		}

		if (name) {
			ast_party_name_free(&ast_channel_redirecting(pbx_channel)->to.name);
			ast_channel_redirecting(pbx_channel)->to.name.str = pbx_strdup(name);
			ast_channel_redirecting(pbx_channel)->to.name.valid = 1;
		}
	}
}

static void sccp_wrapper_asterisk115_updateConnectedLine(constChannelPtr channel, const char *number, const char *name, uint8_t reason)
{
	if (!channel || !channel->owner) {
		return;
	}
	__sccp_asterisk115_updateConnectedLine(channel->owner, number,name, reason);
}

static int sccp_wrapper_asterisk115_sched_add(int when, sccp_sched_cb callback, const void *data)
{
	if (sched) {
		return ast_sched_add(sched, when, callback, data);
	}
	return -1;
}

static int sccp_wrapper_asterisk115_sched_del(int id)
{
	if (sched) {
		return AST_SCHED_DEL(sched, id);
	}
	return -1;
}

static int sccp_wrapper_asterisk115_sched_add_ref(int *id, int when, sccp_sched_cb callback, sccp_channel_t * channel)
{
	if (sched && channel) {
		sccp_channel_t *c = sccp_channel_retain(channel);

		if (c) {
			if ((*id  = ast_sched_add(sched, when, callback, c)) < 0) {
				sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s: sched add id:%d, when:%d, failed\n", c->designator, *id, when);
				sccp_channel_release(&channel);			/* explicit release during failure */
			}
			return *id;
		}
	}
	return -2;
}

static int sccp_wrapper_asterisk115_sched_del_ref(int *id, sccp_channel_t *channel)
{
	if (sched) {
		AST_SCHED_DEL_UNREF(sched, *id, sccp_channel_release(&channel));
		return *id;
	}
	return -2;
}

static int sccp_wrapper_asterisk115_sched_replace_ref(int *id, int when, ast_sched_cb callback, sccp_channel_t *channel)
{
	if (sched) {
		//sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s: (sched_replace_ref) replacing id: %d\n", channel->designator, *id);
		AST_SCHED_REPLACE_UNREF(*id, sched, when, callback, channel, sccp_channel_release((sccp_channel_t **)&_data), sccp_channel_release(&channel), sccp_channel_retain(channel));	/* explicit retain/release */
		//sccp_log(DEBUGCAT_CORE)(VERBOSE_PREFIX_3 "%s: (sched_replace_ref) returning id: %d\n", channel->designator, *id);
		return *id;
	}
	return -2;
}

static long sccp_wrapper_asterisk115_sched_when(int id)
{
	if (sched) {
		return ast_sched_when(sched, id);
	}
	return FALSE;
}

static int sccp_wrapper_asterisk115_sched_wait(int id)
{
	if (sched) {
		return ast_sched_wait(sched);
	}
	return FALSE;
}

static int sccp_wrapper_asterisk115_setCallState(constChannelPtr channel, int state)
{
	sccp_pbx_setcallstate((sccp_channel_t *) channel, state);
	return 0;
}

static boolean_t sccp_asterisk_getRemoteChannel(constChannelPtr channel, PBX_CHANNEL_TYPE ** pbx_channel)
{

	PBX_CHANNEL_TYPE *remotePeer = NULL;

	/*
	struct ast_channel_iterator *iterator = ast_channel_iterator_all_new();
	((struct ao2_iterator *)iterator)->flags |= AO2_ITERATOR_DONTLOCK;
	for (; (remotePeer = ast_channel_iterator_next(iterator)); ast_channel_unref(remotePeer)) {
		if (pbx_find_channel_by_linkid(remotePeer, (void *)ast_channel_linkedid(channel->owner))) {
			break;
		}
	}
	while(!(remotePeer = ast_channel_iterator_next(iterator) ){
		ast_channel_unref(remotePeer);
	}
	ast_channel_iterator_destroy(iterator);
	if (remotePeer) {
		*pbx_channel = remotePeer;
		remotePeer = ast_channel_unref(remotePeer);                     //  should we be releasing th referenec here, it has not been taken explicitly.
		return TRUE;
	}
	*/
	*pbx_channel = remotePeer;
	return FALSE;
}

/*!
 * \brief Send Text to Asterisk Channel
 * \param ast Asterisk Channel as ast_channel
 * \param text Text to be send as char
 * \return Succes as int
 *
 * \called_from_asterisk
 */
static int sccp_pbx_sendtext(PBX_CHANNEL_TYPE * ast, const char *text)
{
	uint8_t instance;

	if (!ast) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: No PBX CHANNEL to send text to\n");
		return -1;
	}

	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));
	if (!c) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: No SCCP CHANNEL to send text to (%s)\n", pbx_channel_name(ast));
		return -1;
	}

	AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));
	if (!d) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: No SCCP DEVICE to send text to (%s)\n", pbx_channel_name(ast));
		return -1;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Sending text %s on %s\n", d->id, text, pbx_channel_name(ast));

	instance = sccp_device_find_index_for_line(d, c->line->name);
	sccp_dev_displayprompt(d, instance, c->callid, (char *) text, 10);
	return 0;
}

/*!
 * \brief Receive First Digit from Asterisk Channel
 * \param ast Asterisk Channel as ast_channel
 * \param digit First Digit as char
 * \return Always Return -1 as int
 *
 * \called_from_asterisk
 */
static int sccp_wrapper_recvdigit_begin(PBX_CHANNEL_TYPE * ast, char digit)
{
	return -1;
}

/*!
 * \brief Receive Last Digit from Asterisk Channel
 * \param ast Asterisk Channel as ast_channel
 * \param digit Last Digit as char
 * \param duration Duration as int
 * \return boolean
 *
 * \called_from_asterisk
 */
static int sccp_wrapper_recvdigit_end(PBX_CHANNEL_TYPE * ast, char digit, unsigned int duration)
{
	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));
	if (c) {
		if (c->dtmfmode == SCCP_DTMFMODE_RFC2833) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Channel(%s) DTMF Mode is RFC2833. Skipping...\n", c->designator, pbx_channel_name(ast));
			return -1;
		}

		AUTO_RELEASE(sccp_device_t, d , sccp_channel_getDevice(c));
		if (!d) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: No SCCP DEVICE to send digit to (%s)\n", pbx_channel_name(ast));
			return -1;
		}

		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Asterisk asked to send dtmf '%d' to channel %s. Trying to send it %s\n", d->id, digit, pbx_channel_name(ast), sccp_dtmfmode2str(d->dtmfmode));
		if (c->state != SCCP_CHANNELSTATE_CONNECTED) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Can't send the dtmf '%d' %c to a not connected channel %s\n", d->id, digit, digit, pbx_channel_name(ast));
			return -1;
		}
		sccp_dev_keypadbutton(d, digit, sccp_device_find_index_for_line(d, c->line->name), c->callid);
	} else {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: No SCCP CHANNEL to send digit to (%s)\n", pbx_channel_name(ast));
		return -1;
	}

	return -1;
}

static PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_findChannelWithCallback(int (*const found_cb) (PBX_CHANNEL_TYPE * c, void *data), void *data, boolean_t lock)
{
	PBX_CHANNEL_TYPE *remotePeer = NULL;

	/*
	struct ast_channel_iterator *iterator = ast_channel_iterator_all_new();
	if (!lock) {
		((struct ao2_iterator *)iterator)->flags |= AO2_ITERATOR_DONTLOCK;
	}
	for (; (remotePeer = ast_channel_iterator_next(iterator)); remotePeer = ast_channel_unref(remotePeer)) {
		if (found_cb(remotePeer, data)) {
			// ast_channel_lock(remotePeer);
			ast_channel_unref(remotePeer);
			break;
		}
	}
	ast_channel_iterator_destroy(iterator);
	*/

	return remotePeer;
}

/*! \brief Set an option on a asterisk channel */
#if 0
static int sccp_wrapper_asterisk115_setOption(PBX_CHANNEL_TYPE * ast, int option, void *data, int datalen)
{
	int res = -1;
	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));

	if (c) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "%s: channel: %s(%s) setOption: %d\n", c->currentDeviceId, sccp_channel_toString(c), pbx_channel_name(ast), option);
		//! if AST_OPTION_FORMAT_READ / AST_OPTION_FORMAT_WRITE are available we might be indication that we can do transcoding (channel.c:set_format). Correct ? */
		switch (option) {
			case AST_OPTION_FORMAT_READ:
				if (c->rtp.audio.instance) {
					res = ast_rtp_instance_set_read_format(c->rtp.audio.instance, (struct ast_format *) data);
				}
				//sccp_wrapper_asterisk115_setReadFormat(c, (struct ast_format *) data);
				break;
			case AST_OPTION_FORMAT_WRITE:
				if (c->rtp.audio.instance) {
					res = ast_rtp_instance_set_write_format(c->rtp.audio.instance, (struct ast_format *) data);
				}
				//sccp_wrapper_asterisk115_setWriteFormat(c, (struct ast_format *) data);
				break;

			case AST_OPTION_MAKE_COMPATIBLE:
				if (c->rtp.audio.instance) {
					res = ast_rtp_instance_make_compatible(ast, c->rtp.audio.instance, (PBX_CHANNEL_TYPE *) data);
				}
				break;
			case AST_OPTION_DIGIT_DETECT:
			case AST_OPTION_SECURE_SIGNALING:
			case AST_OPTION_SECURE_MEDIA:
				res = -1;
				break;
			default:
				break;
		}
	}
	return res;
}
#endif

static void sccp_wrapper_asterisk_set_pbxchannel_linkedid(PBX_CHANNEL_TYPE * pbx_channel, const char *new_linkedid)
{
	if (pbx_channel) {
		if (!strcmp(ast_channel_linkedid(pbx_channel), new_linkedid)) {
			return;
		}
		// ast_cel_check_retire_linkedid(pbx_channel);
		// ast_channel_linkedid_set(pbx_channel, new_linkedid);
		// ast_cel_linkedid_ref(new_linkedid);
	}
};

#define DECLARE_PBX_CHANNEL_STRGET(_field) 									\
static const char *sccp_wrapper_asterisk_get_channel_##_field(constChannelPtr channel)	 		\
{														\
	static const char *empty_channel_##_field = "--no-channel" #_field "--";				\
	if (channel && channel->owner) {											\
		return ast_channel_##_field(channel->owner);							\
	}													\
	return empty_channel_##_field;										\
};

#define DECLARE_PBX_CHANNEL_STRSET(_field)									\
static void sccp_wrapper_asterisk_set_channel_##_field(constChannelPtr channel, const char * (_field))	\
{ 														\
	if (channel && channel->owner) {											\
		ast_channel_##_field##_set(channel->owner, _field);						\
	}													\
};

DECLARE_PBX_CHANNEL_STRGET(name)
    DECLARE_PBX_CHANNEL_STRSET(name)
    DECLARE_PBX_CHANNEL_STRGET(uniqueid)
    DECLARE_PBX_CHANNEL_STRGET(appl)
    DECLARE_PBX_CHANNEL_STRGET(exten)
    DECLARE_PBX_CHANNEL_STRSET(exten)
    DECLARE_PBX_CHANNEL_STRGET(linkedid)
    DECLARE_PBX_CHANNEL_STRGET(context)
    DECLARE_PBX_CHANNEL_STRSET(context)
    DECLARE_PBX_CHANNEL_STRGET(macroexten)
    DECLARE_PBX_CHANNEL_STRSET(macroexten)
    DECLARE_PBX_CHANNEL_STRGET(macrocontext)
    DECLARE_PBX_CHANNEL_STRSET(macrocontext)
    DECLARE_PBX_CHANNEL_STRGET(call_forward)
    DECLARE_PBX_CHANNEL_STRSET(call_forward)

static void sccp_wrapper_asterisk_set_channel_linkedid(constChannelPtr channel, const char *new_linkedid)
{
	if (channel && channel->owner) {
		sccp_wrapper_asterisk_set_pbxchannel_linkedid(channel->owner, new_linkedid);
	}
};

static enum ast_channel_state sccp_wrapper_asterisk_get_channel_state(constChannelPtr channel)
{
	if (channel && channel->owner) {
		return ast_channel_state(channel->owner);
	}
	return 0;
}

static const struct ast_pbx *sccp_wrapper_asterisk_get_channel_pbx(constChannelPtr channel)
{
	if (channel && channel->owner) {
		return ast_channel_pbx(channel->owner);
	}
	return NULL;
}

static int sccp_pbx_sendHTML(PBX_CHANNEL_TYPE * ast, int subclass, const char *data, int datalen)
{
	int res = -1;
	if (!datalen || sccp_strlen_zero(data) || !(!strncmp(data, "http://", 7) || !strncmp(data, "file://", 7) || !strncmp(data, "ftp://", 6))) {
		pbx_log(LOG_NOTICE, "SCCP: Received a non valid URL\n");
		return res;
	}
	struct ast_frame fr;

	AUTO_RELEASE(sccp_channel_t, c , get_sccp_channel_from_pbx_channel(ast));
	if (c) {
		AUTO_RELEASE(sccp_device_t, d , c->getDevice(c));
		if (d) {
			memset(&fr, 0, sizeof(fr));
			fr.frametype = AST_FRAME_HTML;
			fr.data.ptr = (char *) data;
			fr.src = "SCCP Send URL";
			fr.datalen = datalen;

			sccp_push_result_t pushResult = d->pushURL(d, data, 1, SKINNY_TONE_ZIP);

			if (SCCP_PUSH_RESULT_SUCCESS == pushResult) {
				fr.subclass.integer = AST_HTML_LDCOMPLETE;
			} else {
				fr.subclass.integer = AST_HTML_NOSUPPORT;
			}
			ast_queue_frame(ast, ast_frisolate(&fr));
			res = 0;
		}
	}
	return res;
}

/*!
 * \brief Queue a control frame
 * \param pbx_channel PBX Channel
 * \param control as Asterisk Control Frame Type
 */
int sccp_asterisk_queue_control(const PBX_CHANNEL_TYPE * pbx_channel, enum ast_control_frame_type control)
{
	struct ast_frame f = { AST_FRAME_CONTROL,.subclass.integer = control };
	return ast_queue_frame((PBX_CHANNEL_TYPE *) pbx_channel, &f);
}

/*!
 * \brief Queue a control frame with payload
 * \param pbx_channel PBX Channel
 * \param control as Asterisk Control Frame Type
 * \param data Payload
 * \param datalen Payload Length
 */
int sccp_asterisk_queue_control_data(const PBX_CHANNEL_TYPE * pbx_channel, enum ast_control_frame_type control, const void *data, size_t datalen)
{
	struct ast_frame f = { AST_FRAME_CONTROL,.subclass.integer = control,.data.ptr = (void *) data,.datalen = datalen };
	return ast_queue_frame((PBX_CHANNEL_TYPE *) pbx_channel, &f);
}

/*!
 * \brief Get Hint Extension State and return the matching Busy Lamp Field State
 */
static skinny_busylampfield_state_t sccp_wrapper_asterisk115_getExtensionState(const char *extension, const char *context)
{
	skinny_busylampfield_state_t result = SKINNY_BLF_STATUS_UNKNOWN;

	if (sccp_strlen_zero(extension) || sccp_strlen_zero(context)) {
		pbx_log(LOG_ERROR, "SCCP: iPbx.getExtensionState: Either extension:'%s' or context:;%s' provided is empty\n", extension, context);
		return result;
	}

	int state = ast_extension_state(NULL, context, extension);

	switch (state) {
		case AST_EXTENSION_REMOVED:
		case AST_EXTENSION_DEACTIVATED:
		case AST_EXTENSION_UNAVAILABLE:
			result = SKINNY_BLF_STATUS_UNKNOWN;
			break;
		case AST_EXTENSION_NOT_INUSE:
			result = SKINNY_BLF_STATUS_IDLE;
			break;
		case AST_EXTENSION_INUSE:
		case AST_EXTENSION_ONHOLD:
		case AST_EXTENSION_ONHOLD + AST_EXTENSION_INUSE:
		case AST_EXTENSION_BUSY:
			result = SKINNY_BLF_STATUS_INUSE;
			break;
		case AST_EXTENSION_RINGING + AST_EXTENSION_INUSE:
		case AST_EXTENSION_RINGING:
			result = SKINNY_BLF_STATUS_ALERTING;
			break;
	}
	sccp_log(DEBUGCAT_HINT) (VERBOSE_PREFIX_4 "SCCP: (getExtensionState) extension: %s@%s, extension_state: '%s (%d)' -> blf state '%d'\n", extension, context, ast_extension_state2str(state), state, result);
	return result;
}

static struct ast_endpoint *sccp_wrapper_asterisk115_endpoint_create(const char *tech, const char *resource)
{
	return ast_endpoint_create(tech, resource);
}

static void sccp_wrapper_asterisk115_endpoint_online(struct ast_endpoint *endpoint, const char *address)
{
	RAII_VAR(struct ast_json *, blob, NULL, ast_json_unref);
	ast_endpoint_set_state(endpoint, AST_ENDPOINT_ONLINE);
	blob = ast_json_pack("{s: s, s: s}", "peer_status", "Registered", "address", address);
	ast_endpoint_blob_publish(endpoint, ast_endpoint_state_type(), blob);
}

static void sccp_wrapper_asterisk115_endpoint_offline(struct ast_endpoint *endpoint, const char *cause)
{
	RAII_VAR(struct ast_json *, blob, NULL, ast_json_unref);
	ast_endpoint_set_state(endpoint, AST_ENDPOINT_OFFLINE);
	blob = ast_json_pack("{s: s, s: s}", "peer_status", "Unregistered", "cause", cause);
	ast_endpoint_blob_publish(endpoint, ast_endpoint_state_type(), blob);
}

static void sccp_wrapper_asterisk115_endpoint_shutdown(struct ast_endpoint **endpoint)
{
	ast_endpoint_shutdown(*endpoint);
	*endpoint = NULL;
}

static int sccp_wrapper_asterisk115_dumpchan(struct ast_channel *c, char *buf, size_t size)
{
	long elapsed_seconds = 0;
	int hour = 0, min = 0, sec = 0;
	char cgrp[256];
	char pgrp[256];
	struct ast_str *write_transpath = ast_str_alloca(256);
	struct ast_str *read_transpath = ast_str_alloca(256);
	struct ast_bridge *bridge;
	struct ast_str *vars = ast_str_thread_get(&ast_str_thread_global_buf, 16);
	struct ast_str *codec_buf = ast_str_alloca(64);

	memset(buf, 0, size);
	if (!c) {
		return -1;
	}

	elapsed_seconds = ast_channel_get_duration(c);
	hour = elapsed_seconds / 3600;
	min = (elapsed_seconds % 3600) / 60;
	sec = elapsed_seconds % 60;

	ast_channel_lock(c);
	bridge = ast_channel_get_bridge(c);
	ast_channel_unlock(c);

	pbx_builtin_serialize_variables(c, &vars);

	snprintf(buf, size, "Name=               %s\n"
				"Type=               %s\n"
				"UniqueID=           %s\n"
				"LinkedID=           %s\n"
				"CallerIDNum=        %s\n"
				"CallerIDName=       %s\n"
				"ConnectedLineIDNum= %s\n"
				"ConnectedLineIDName=%s\n"
				"DNIDDigits=         %s\n"
				"RDNIS=              %s\n"
				"Parkinglot=         %s\n"
				"Language=           %s\n"
				"State=              %s (%d)\n"
				"Rings=              %d\n"
				"NativeFormat=       %s\n"
				"WriteFormat=        %s\n"
				"ReadFormat=         %s\n"
				"RawWriteFormat=     %s\n"
				"RawReadFormat=      %s\n"
				"WriteTranscode=     %s %s\n"
				"ReadTranscode=      %s %s\n"
				"1stFileDescriptor=  %d\n"
				"Framesin=           %d %s\n"
				"Framesout=          %d %s\n"
				"TimetoHangup=       %ld\n"
				"ElapsedTime=        %dh%dm%ds\n"
				"BridgeID=           %s\n"
				"Context=            %s\n"
				"Extension=          %s\n"
				"Priority=           %d\n"
				"CallGroup=          %s\n"
				"PickupGroup=        %s\n"
				"Application=        %s\n"
				"Data=               %s\n"
				"Blocking_in=        %s\n"
				"Variables=          %s\n",
		ast_channel_name(c),
		ast_channel_tech(c)->type,
		ast_channel_uniqueid(c),
		ast_channel_linkedid(c),
		S_COR(ast_channel_caller(c)->id.number.valid, ast_channel_caller(c)->id.number.str, "(N/A)"), 
		S_COR(ast_channel_caller(c)->id.name.valid, ast_channel_caller(c)->id.name.str, "(N/A)"), 
		S_COR(ast_channel_connected(c)->id.number.valid, ast_channel_connected(c)->id.number.str, "(N/A)"), 
		S_COR(ast_channel_connected(c)->id.name.valid, ast_channel_connected(c)->id.name.str, "(N/A)"), 
		S_OR(ast_channel_dialed(c)->number.str, "(N/A)"), 
		S_COR(ast_channel_redirecting(c)->from.number.valid, ast_channel_redirecting(c)->from.number.str, "(N/A)"), 
		ast_channel_parkinglot(c), 
		ast_channel_language(c), 
		ast_state2str(ast_channel_state(c)), 
		ast_channel_state(c), 
		ast_channel_rings(c), 
		ast_format_cap_get_names(ast_channel_nativeformats(c), &codec_buf),	//ast_getformatname_multiple(nf, sizeof(nf), ast_channel_nativeformats(c)),
		ast_format_get_name(ast_channel_writeformat(c)),
		ast_format_get_name(ast_channel_readformat(c)),
		ast_format_get_name(ast_channel_rawwriteformat(c)),
		ast_format_get_name(ast_channel_rawreadformat(c)),
		ast_channel_writetrans(c) ? "Yes" : "No",
		ast_translate_path_to_str(ast_channel_writetrans(c), &write_transpath),
		ast_channel_readtrans(c) ? "Yes" : "No",
		ast_translate_path_to_str(ast_channel_readtrans(c), &read_transpath),
		ast_channel_fd(c, 0),
		ast_channel_fin(c) & ~DEBUGCHAN_FLAG, (ast_channel_fin(c) & DEBUGCHAN_FLAG) ? " (DEBUGGED)" : "",
		ast_channel_fout(c) & ~DEBUGCHAN_FLAG, (ast_channel_fout(c) & DEBUGCHAN_FLAG) ? " (DEBUGGED)" : "",
		(long) ast_channel_whentohangup(c)->tv_sec,
		hour,
		min,
		sec,
		bridge ? bridge->uniqueid : "(Not bridged)",
		ast_channel_context(c), 
		ast_channel_exten(c), 
		ast_channel_priority(c), 
		ast_print_group(cgrp, sizeof(cgrp), ast_channel_callgroup(c)), 
		ast_print_group(pgrp, sizeof(pgrp), ast_channel_pickupgroup(c)), 
		ast_channel_appl(c) ? ast_channel_appl(c) : "(N/A)", 
		ast_channel_data(c) ? S_OR(ast_channel_data(c), "(Empty)") : "(None)", 
		(ast_test_flag(ast_channel_flags(c), AST_FLAG_BLOCKING) ? ast_channel_blockproc(c) : "(Not Blocking)"), 
		ast_str_buffer(vars)
	);

	ao2_cleanup(bridge);
	return 0;
}

static boolean_t sccp_wrapper_asterisk115_channelIsBridged(sccp_channel_t * channel)
{
	boolean_t res = FALSE;

	if (!channel || !channel->owner) {
		return res;
	}
	ast_channel_lock(channel->owner);
	res = ast_channel_is_bridged(channel->owner);
	ast_channel_unlock(channel->owner);
	return res;
}

static PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_getBridgeChannel(PBX_CHANNEL_TYPE * pbx_channel)
{
	PBX_CHANNEL_TYPE *bridgePeer = NULL;
	if (pbx_channel) {
		bridgePeer = ast_channel_bridge_peer(pbx_channel);		/* return pbx_channel_ref +1 */
	}
	return bridgePeer;
}

static boolean_t sccp_wrapper_asterisk115_attended_transfer(sccp_channel_t * destination_channel, sccp_channel_t * source_channel)
{
	enum ast_transfer_result res;
	// possibly move transfer related callinfo updates here
	if (!destination_channel || !source_channel || !destination_channel->owner || !source_channel->owner) {
		return FALSE;
	}
	PBX_CHANNEL_TYPE *pbx_destination_local_channel = destination_channel->owner;
	PBX_CHANNEL_TYPE *pbx_source_local_channel = source_channel->owner;

	res = ast_bridge_transfer_attended(pbx_destination_local_channel, pbx_source_local_channel);
	if (res != AST_BRIDGE_TRANSFER_SUCCESS) {
		pbx_log(LOG_ERROR, "%s: Failed to transfer %s to %s (%u)\n", source_channel->designator, source_channel->designator, destination_channel->designator, res);
		ast_queue_control(pbx_destination_local_channel, AST_CONTROL_HOLD);		
		return FALSE;
	}
	return TRUE;
}

/*!
 * \brief using RTP Glue Engine
 */
#if defined(__cplusplus) || defined(c_plusplus)
struct ast_rtp_glue sccp_rtp = {
	/* *INDENT-OFF* */
	type:	SCCP_TECHTYPE_STR,
	mod:	NULL,
	get_rtp_info:sccp_wrapper_asterisk115_get_rtp_info,
	//allow_rtp_remote:sccp_wrapper_asterisk115_allow_rtp_remote, 		/* check c->directmedia and return 1 if ok */
	get_vrtp_info:sccp_wrapper_asterisk115_get_vrtp_info,
	//allow_vrtp_remote:sccp_wrapper_asterisk115_allow_vrtp_remote, 	/* check c->directmedia and return 1 if ok */
	get_trtp_info:NULL,
	update_peer:sccp_wrapper_asterisk115_update_rtp_peer,
	get_codec:sccp_wrapper_asterisk115_getCodec,
	/* *INDENT-ON* */
};
#else
struct ast_rtp_glue sccp_rtp = {
	.type = SCCP_TECHTYPE_STR,
	.get_rtp_info = sccp_wrapper_asterisk115_get_rtp_info,
	//.allow_rtp_remote = sccp_wrapper_asterisk115_allow_rtp_remote, 	/* check c->directmedia and return 1 if ok */
	.get_vrtp_info = sccp_wrapper_asterisk115_get_vrtp_info,
	//.allow_vrtp_remote = sccp_wrapper_asterisk115_allow_vrtp_remote, 	/* check c->directmedia and return 1 if ok */
	.update_peer = sccp_wrapper_asterisk115_update_rtp_peer,
	.get_codec = sccp_wrapper_asterisk115_getCodec,
};
#endif

#ifdef HAVE_PBX_MESSAGE_H
#include <asterisk/message.h>
static int sccp_asterisk_message_send(const struct ast_msg *msg, const char *to, const char *from)
{
	char *lineName;
	const char *messageText = ast_msg_get_body(msg);
	int res = -1;

	lineName = pbx_strdupa(to);
	if (strchr(lineName, '@')) {
		strsep(&lineName, "@");
	} else {
		strsep(&lineName, ":");
	}
	if (sccp_strlen_zero(lineName)) {
		pbx_log(LOG_WARNING, "MESSAGE(to) is invalid for SCCP - '%s'\n", to);
		return -1;
	}

	AUTO_RELEASE(sccp_line_t, line , sccp_line_find_byname(lineName, FALSE));
	if (!line) {
		pbx_log(LOG_WARNING, "line '%s' not found\n", lineName);
		return -1;
	}

	/** \todo move this to line implementation */
	sccp_linedevices_t *linedevice = NULL;
	sccp_push_result_t pushResult;

	SCCP_LIST_LOCK(&line->devices);
	SCCP_LIST_TRAVERSE(&line->devices, linedevice, list) {
		pushResult = linedevice->device->pushTextMessage(linedevice->device, messageText, from, 1, SKINNY_TONE_ZIP);
		if (SCCP_PUSH_RESULT_SUCCESS == pushResult) {
			res = 0;
		}
	}
	SCCP_LIST_UNLOCK(&line->devices);

	return res;
}

#if defined(__cplusplus) || defined(c_plusplus)
static const struct ast_msg_tech sccp_msg_tech = {
	/* *INDENT-OFF* */
	name:	"sccp",
	msg_send:sccp_asterisk_message_send,
	/* *INDENT-ON* */
};
#else
static const struct ast_msg_tech sccp_msg_tech = {
	/* *INDENT-OFF* */
	.name = "sccp",
	.msg_send = sccp_asterisk_message_send,
	/* *INDENT-ON* */
};
#endif

#endif

/*!
 * \brief pbx_manager_register
 *
 * \note this functions needs to be defined here, because it depends on the static declaration of ast_module_info->self
 */
int pbx_manager_register(const char *action, int authority, int (*func) (struct mansession * s, const struct message * m), const char *synopsis, const char *description)
{
	return ast_manager_register2(action, authority, func, ast_module_info->self, synopsis, description);
}

static int sccp_wrapper_register_application(const char *app_name, int (*execute)(struct ast_channel *, const char *))
{
	return ast_register_application2(app_name, execute, NULL, NULL, ast_module_info->self);
}

static int sccp_wrapper_unregister_application(const char *app_name)
{
	return ast_unregister_application(app_name);
}

static int sccp_wrapper_register_function(struct pbx_custom_function *custom_function)
{
	return __ast_custom_function_register(custom_function, ast_module_info->self);
}

static int sccp_wrapper_unregister_function(struct pbx_custom_function *custom_function)
{
	return ast_custom_function_unregister(custom_function);
}

static boolean_t sccp_wrapper_asterisk115_setLanguage(PBX_CHANNEL_TYPE * pbxChannel, const char *language)
{

	ast_channel_language_set(pbxChannel, language);
	return TRUE;
}

#if defined(__cplusplus) || defined(c_plusplus)
//sccp_pbx_cb sccp_pbx = {
const PbxInterface iPbx = {
	/* *INDENT-OFF* */

	alloc_pbxChannel:		sccp_wrapper_asterisk115_allocPBXChannel,
	set_callstate:			sccp_wrapper_asterisk115_setCallState,
	checkhangup:			sccp_wrapper_asterisk115_checkHangup,
	hangup:				NULL,
	extension_status:		sccp_wrapper_asterisk115_extensionStatus,

	setPBXChannelLinkedId:		sccp_wrapper_asterisk_set_pbxchannel_linkedid,
	/** get channel by name */
	getChannelByName:		sccp_wrapper_asterisk115_getChannelByName,
	getRemoteChannel:		sccp_asterisk_getRemoteChannel,
	getChannelByCallback:		NULL,

	getChannelLinkedId:		sccp_wrapper_asterisk_get_channel_linkedid,
	setChannelLinkedId:		sccp_wrapper_asterisk_set_channel_linkedid,
	getChannelName:			sccp_wrapper_asterisk_get_channel_name,
	getChannelUniqueID:		sccp_wrapper_asterisk_get_channel_uniqueid,
	getChannelExten:		sccp_wrapper_asterisk_get_channel_exten,
	setChannelExten:		sccp_wrapper_asterisk_set_channel_exten,
	getChannelContext:		sccp_wrapper_asterisk_get_channel_context,
	setChannelContext:		sccp_wrapper_asterisk_set_channel_context,
	getChannelMacroExten:		sccp_wrapper_asterisk_get_channel_macroexten,
	setChannelMacroExten:		sccp_wrapper_asterisk_set_channel_macroexten,
	getChannelMacroContext:		sccp_wrapper_asterisk_get_channel_macrocontext,
	setChannelMacroContext:		sccp_wrapper_asterisk_set_channel_macrocontext,
	getChannelCallForward:		sccp_wrapper_asterisk_get_channel_call_forward,
	setChannelCallForward:		sccp_wrapper_asterisk_set_channel_call_forward,

	getChannelAppl:			sccp_wrapper_asterisk_get_channel_appl,
	getChannelState:		sccp_wrapper_asterisk_get_channel_state,
	getChannelPbx:			sccp_wrapper_asterisk_get_channel_pbx,

	set_nativeAudioFormats:		sccp_wrapper_asterisk115_setNativeAudioFormats,
	set_nativeVideoFormats:		sccp_wrapper_asterisk115_setNativeVideoFormats,

	getPeerCodecCapabilities:	NULL,
	send_digit:			sccp_wrapper_sendDigit,
	send_digits:			sccp_wrapper_sendDigits,

	sched_add:			sccp_wrapper_asterisk115_sched_add,
	sched_del:			sccp_wrapper_asterisk115_sched_del,
	sched_add_ref:			sccp_wrapper_asterisk115_sched_add_ref,
	sched_del_ref:			sccp_wrapper_asterisk115_sched_del_ref,
	sched_replace_ref:		sccp_wrapper_asterisk115_sched_replace_ref,
	sched_when:			sccp_wrapper_asterisk115_sched_when,
	sched_wait:			sccp_wrapper_asterisk115_sched_wait,

	/* rtp */
	rtp_getPeer:			sccp_wrapper_asterisk115_rtpGetPeer,
	rtp_getUs:			sccp_wrapper_asterisk115_rtpGetUs,
	rtp_setPhoneAddress:		sccp_wrapper_asterisk115_setPhoneRTPAddress,
	rtp_setWriteFormat:		sccp_wrapper_asterisk115_setWriteFormat,
	rtp_setReadFormat:		sccp_wrapper_asterisk115_setReadFormat,
	rtp_destroy:			sccp_wrapper_asterisk115_destroyRTP,
	rtp_stop:			ast_rtp_instance_stop(rtp),
	rtp_codec:			NULL,
	rtp_create_instance:		sccp_wrapper_asterisk115_createRtpInstance,
	rtp_get_payloadType:		sccp_wrapper_asterisk115_get_payloadType,
	rtp_get_sampleRate:		sccp_wrapper_asterisk115_get_sampleRate,
	rtp_bridgePeers:		NULL,

	/* callerid */
	get_callerid_name:		sccp_wrapper_asterisk115_callerid_name,
	get_callerid_number:		sccp_wrapper_asterisk115_callerid_number,
	get_callerid_ton:		sccp_wrapper_asterisk115_callerid_ton,
	get_callerid_ani:		sccp_wrapper_asterisk115_callerid_ani,
	get_callerid_subaddr:		sccp_wrapper_asterisk115_callerid_subaddr,
	get_callerid_dnid:		sccp_wrapper_asterisk115_callerid_dnid,
	get_callerid_rdnis:		sccp_wrapper_asterisk115_callerid_rdnis,
	get_callerid_presentation:	sccp_wrapper_asterisk115_callerid_presentation,

	set_callerid_name:		sccp_wrapper_asterisk115_setCalleridName,
	set_callerid_number:		sccp_wrapper_asterisk115_setCalleridNumber,
	set_callerid_ani:		sccp_wrapper_asterisk115_setCalleridAni,
	set_callerid_dnid:		NULL,
	set_callerid_redirectingParty:	sccp_wrapper_asterisk115_setRedirectingParty,
	set_callerid_redirectedParty:	sccp_wrapper_asterisk115_setRedirectedParty,
	set_callerid_presentation:	sccp_wrapper_asterisk115_setCalleridPresentation,

	set_dialed_number:		sccp_wrapper_asterisk115_setDialedNumber,
	set_connected_line:		sccp_wrapper_asterisk115_updateConnectedLine,
	sendRedirectedUpdate:		sccp_asterisk_sendRedirectedUpdate,

	/* feature section */
	feature_park:			sccp_wrapper_asterisk115_park,
	feature_stopMusicOnHold:	NULL,
	feature_addToDatabase:		sccp_asterisk_addToDatabase,
	feature_getFromDatabase:	sccp_asterisk_getFromDatabase,
	feature_removeFromDatabase:	sccp_asterisk_removeFromDatabase,
	feature_removeTreeFromDatabase:	sccp_asterisk_removeTreeFromDatabase,
	feature_monitor:		sccp_wrapper_asterisk_featureMonitor,
	getFeatureExtension:		sccp_wrapper_asterisk115_getFeatureExtension,
	getPickupExtension:		sccp_wrapper_asterisk115_getPickupExtension,

	eventSubscribe:			NULL,
	findChannelByCallback:		sccp_wrapper_asterisk115_findChannelWithCallback,

	moh_start:			sccp_asterisk_moh_start,
	moh_stop:			sccp_asterisk_moh_stop,
	queue_control:			sccp_asterisk_queue_control,
	queue_control_data:		sccp_asterisk_queue_control_data,

	allocTempPBXChannel:		sccp_wrapper_asterisk115_allocTempPBXChannel,
	masqueradeHelper:		sccp_wrapper_asterisk115_masqueradeHelper,
	requestAnnouncementChannel:	sccp_wrapper_asterisk115_requestAnnouncementChannel,

	set_language:			sccp_wrapper_asterisk115_setLanguage,

	getExtensionState:		sccp_wrapper_asterisk115_getExtensionState,
	findPickupChannelByExtenLocked:	sccp_wrapper_asterisk115_findPickupChannelByExtenLocked,
	findPickupChannelByGroupLocked:	sccp_wrapper_asterisk115_findPickupChannelByGroupLocked,

 	endpoint_create:		sccp_wrapper_asterisk115_endpoint_create,
	endpoint_online:		sccp_wrapper_asterisk115_endpoint_online,
	endpoint_offline:		sccp_wrapper_asterisk115_endpoint_offline,
	endpoint_shutdown:		sccp_wrapper_asterisk115_endpoint_shutdown,

	set_owner:			sccp_wrapper_asterisk115_setOwner,
	removeTimingFD:			sccp_wrapper_asterisk115_removeTimingFD,
	dumpchan:			sccp_wrapper_asterisk115_dumpchan,
	channel_is_bridged:		sccp_wrapper_asterisk115_channelIsBridged,
	get_bridged_channel:		sccp_wrapper_asterisk115_getBridgeChannel,
	get_underlying_channel:		sccp_wrapper_asterisk115_getBridgeChannel,
	attended_transfer:		sccp_wrapper_asterisk115_attended_transfer,

	set_callgroup:			sccp_wrapper_asterisk_set_callgroup,
	set_pickupgroup:		sccp_wrapper_asterisk_set_pickupgroup,
	set_named_callgroups:		sccp_wrapper_asterisk_set_named_callgroups,
	set_named_pickupgroups:		sccp_wrapper_asterisk_set_named_pickupgroups,

	register_application:		sccp_wrapper_register_application,
	unregister_application:		sccp_wrapper_unregister_application,
	register_function:		sccp_wrapper_register_function,
	unregister_function:		sccp_wrapper_unregister_function,
	/* *INDENT-ON* */
};

#else

/*!
 * \brief SCCP - PBX Callback Functions
 * (Decoupling Tight Dependencies on Asterisk Functions)
 */
//struct sccp_pbx_cb sccp_pbx = {
const PbxInterface iPbx = {
	/* *INDENT-OFF* */

	/* channel */
	.alloc_pbxChannel 		= sccp_wrapper_asterisk115_allocPBXChannel,
	.extension_status 		= sccp_wrapper_asterisk115_extensionStatus,
	.setPBXChannelLinkedId		= sccp_wrapper_asterisk_set_pbxchannel_linkedid,

	.getChannelByName 		= sccp_wrapper_asterisk115_getChannelByName,
	.getChannelLinkedId		= sccp_wrapper_asterisk_get_channel_linkedid,
	.setChannelLinkedId		= sccp_wrapper_asterisk_set_channel_linkedid,
	.getChannelName			= sccp_wrapper_asterisk_get_channel_name,
	.setChannelName			= sccp_wrapper_asterisk_set_channel_name,
	.getChannelUniqueID		= sccp_wrapper_asterisk_get_channel_uniqueid,
	.getChannelExten		= sccp_wrapper_asterisk_get_channel_exten,
	.setChannelExten		= sccp_wrapper_asterisk_set_channel_exten,
	.getChannelContext		= sccp_wrapper_asterisk_get_channel_context,
	.setChannelContext		= sccp_wrapper_asterisk_set_channel_context,
	.getChannelMacroExten		= sccp_wrapper_asterisk_get_channel_macroexten,
	.setChannelMacroExten		= sccp_wrapper_asterisk_set_channel_macroexten,
	.getChannelMacroContext		= sccp_wrapper_asterisk_get_channel_macrocontext,
	.setChannelMacroContext		= sccp_wrapper_asterisk_set_channel_macrocontext,
	.getChannelCallForward		= sccp_wrapper_asterisk_get_channel_call_forward,
	.setChannelCallForward		= sccp_wrapper_asterisk_set_channel_call_forward,

	.getChannelAppl			= sccp_wrapper_asterisk_get_channel_appl,
	.getChannelState		= sccp_wrapper_asterisk_get_channel_state,
	.getChannelPbx			= sccp_wrapper_asterisk_get_channel_pbx,

	.getRemoteChannel		= sccp_asterisk_getRemoteChannel,
	.checkhangup			= sccp_wrapper_asterisk115_checkHangup,

	/* digits */
	.send_digits 			= sccp_wrapper_sendDigits,
	.send_digit 			= sccp_wrapper_sendDigit,

	/* schedulers */
	.sched_add			= sccp_wrapper_asterisk115_sched_add,
	.sched_del			= sccp_wrapper_asterisk115_sched_del,
	.sched_add_ref			= sccp_wrapper_asterisk115_sched_add_ref,
	.sched_del_ref			= sccp_wrapper_asterisk115_sched_del_ref,
	.sched_replace_ref		= sccp_wrapper_asterisk115_sched_replace_ref,
	.sched_when 			= sccp_wrapper_asterisk115_sched_when,
	.sched_wait 			= sccp_wrapper_asterisk115_sched_wait,

	/* callstate / indicate */
	.set_callstate 			= sccp_wrapper_asterisk115_setCallState,

	/* codecs */
	.set_nativeAudioFormats 	= sccp_wrapper_asterisk115_setNativeAudioFormats,
	.set_nativeVideoFormats 	= sccp_wrapper_asterisk115_setNativeVideoFormats,

	/* rtp */
	.rtp_getPeer			= sccp_wrapper_asterisk115_rtpGetPeer,
	.rtp_getUs 			= sccp_wrapper_asterisk115_rtpGetUs,
	.rtp_stop			= ast_rtp_instance_stop,
	.rtp_create_instance		= sccp_wrapper_asterisk115_createRtpInstance,
	.rtp_get_payloadType 		= sccp_wrapper_asterisk115_get_payloadType,
	.rtp_get_sampleRate 		= sccp_wrapper_asterisk115_get_sampleRate,
	.rtp_destroy 			= sccp_wrapper_asterisk115_destroyRTP,
	.rtp_setWriteFormat 		= sccp_wrapper_asterisk115_setWriteFormat,
	.rtp_setReadFormat 		= sccp_wrapper_asterisk115_setReadFormat,
	.rtp_setPhoneAddress		= sccp_wrapper_asterisk115_setPhoneRTPAddress,

	/* callerid */
	.get_callerid_name 		= sccp_wrapper_asterisk115_callerid_name,
	.get_callerid_number 		= sccp_wrapper_asterisk115_callerid_number,
	.get_callerid_ton 		= sccp_wrapper_asterisk115_callerid_ton,
	.get_callerid_ani 		= sccp_wrapper_asterisk115_callerid_ani,
	.get_callerid_subaddr 		= sccp_wrapper_asterisk115_callerid_subaddr,
	.get_callerid_dnid 		= sccp_wrapper_asterisk115_callerid_dnid,
	.get_callerid_rdnis 		= sccp_wrapper_asterisk115_callerid_rdnis,
	.get_callerid_presentation 	= sccp_wrapper_asterisk115_callerid_presentation,

	.set_callerid_name 		= sccp_wrapper_asterisk115_setCalleridName,
	.set_callerid_number 		= sccp_wrapper_asterisk115_setCalleridNumber,
	.set_callerid_ani 		= sccp_wrapper_asterisk115_setCalleridAni,
	.set_callerid_dnid 		= NULL,						//! \todo implement callback
	.set_callerid_redirectingParty 	= sccp_wrapper_asterisk115_setRedirectingParty,
	.set_callerid_redirectedParty 	= sccp_wrapper_asterisk115_setRedirectedParty,
	.set_callerid_presentation 	= sccp_wrapper_asterisk115_setCalleridPresentation,
	.set_dialed_number		= sccp_wrapper_asterisk115_setDialedNumber,
	.set_connected_line		= sccp_wrapper_asterisk115_updateConnectedLine,
	.sendRedirectedUpdate		= sccp_asterisk_sendRedirectedUpdate,

	/* database */
	.feature_addToDatabase 		= sccp_asterisk_addToDatabase,
	.feature_getFromDatabase 	= sccp_asterisk_getFromDatabase,
	.feature_removeFromDatabase     = sccp_asterisk_removeFromDatabase,
	.feature_removeTreeFromDatabase = sccp_asterisk_removeTreeFromDatabase,
	.feature_monitor		= sccp_wrapper_asterisk_featureMonitor,

	.feature_park			= sccp_wrapper_asterisk115_park,
	.getFeatureExtension		= sccp_wrapper_asterisk115_getFeatureExtension,
	.getPickupExtension		= sccp_wrapper_asterisk115_getPickupExtension,

	.findChannelByCallback		= sccp_wrapper_asterisk115_findChannelWithCallback,

	.moh_start			= sccp_asterisk_moh_start,
	.moh_stop			= sccp_asterisk_moh_stop,
	.queue_control			= sccp_asterisk_queue_control,
	.queue_control_data		= sccp_asterisk_queue_control_data,

	.allocTempPBXChannel		= sccp_wrapper_asterisk115_allocTempPBXChannel,
	.masqueradeHelper		= sccp_wrapper_asterisk115_masqueradeHelper,
	.requestAnnouncementChannel	= sccp_wrapper_asterisk115_requestAnnouncementChannel,

	.set_language			= sccp_wrapper_asterisk115_setLanguage,

	.getExtensionState		= sccp_wrapper_asterisk115_getExtensionState,
	.findPickupChannelByExtenLocked	= sccp_wrapper_asterisk115_findPickupChannelByExtenLocked,
	.findPickupChannelByGroupLocked	= sccp_wrapper_asterisk115_findPickupChannelByGroupLocked,

	.endpoint_create		= sccp_wrapper_asterisk115_endpoint_create,
	.endpoint_online		= sccp_wrapper_asterisk115_endpoint_online,
	.endpoint_offline		= sccp_wrapper_asterisk115_endpoint_offline,
	.endpoint_shutdown		= sccp_wrapper_asterisk115_endpoint_shutdown,

	.set_owner			= sccp_wrapper_asterisk115_setOwner,
	.removeTimingFD			= sccp_wrapper_asterisk115_removeTimingFD,
	.dumpchan			= sccp_wrapper_asterisk115_dumpchan,
	.channel_is_bridged		= sccp_wrapper_asterisk115_channelIsBridged,
	.get_bridged_channel		= sccp_wrapper_asterisk115_getBridgeChannel,
	.get_underlying_channel		= sccp_wrapper_asterisk115_getBridgeChannel,
	.attended_transfer		= sccp_wrapper_asterisk115_attended_transfer,

	.set_callgroup			= sccp_wrapper_asterisk_set_callgroup,
	.set_pickupgroup		= sccp_wrapper_asterisk_set_pickupgroup,
	.set_named_callgroups		= sccp_wrapper_asterisk_set_named_callgroups,
	.set_named_pickupgroups		= sccp_wrapper_asterisk_set_named_pickupgroups,

	.register_application		= sccp_wrapper_register_application,
	.unregister_application		= sccp_wrapper_unregister_application,
	.register_function		= sccp_wrapper_register_function,
	.unregister_function		= sccp_wrapper_unregister_function,
	/* *INDENT-ON* */
};
#endif

static int register_channel_tech(struct ast_channel_tech *tech)
{
	tech->capabilities = ast_format_cap_alloc(0);
	if (!tech->capabilities) {
		ao2_cleanup(tech->capabilities);
		return -1;
	}
	ast_format_cap_append_by_type(tech->capabilities, AST_MEDIA_TYPE_AUDIO);
	ast_format_cap_append_by_type(tech->capabilities, AST_MEDIA_TYPE_VIDEO);
	//ast_format_cap_append_by_type(tech->capabilities, AST_MEDIA_TYPE_TEXT);

	if (ast_channel_register(tech)) {
		ast_log(LOG_ERROR, "Unable to register channel technology %s(%s).\n", tech->type, tech->description);
		return -1;
	}
	return 0;
}

static void unregister_channel_tech(struct ast_channel_tech *tech)
{
	ast_channel_unregister(tech);
	if (tech->capabilities) {
		ao2_ref(tech->capabilities, -1);
	}
	tech->capabilities = NULL;
	tech = NULL;
}

static int unload_module(void)
{
	pbx_log(LOG_NOTICE, "SCCP: Module Unload\n");
	sccp_preUnload();
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Unregister SCCP RTP protocol\n");
	ast_rtp_glue_unregister(&sccp_rtp);
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_2 "SCCP: Unregister SCCP Channel Tech\n");

	unregister_channel_tech(&sccp_tech);
	sccp_unregister_dialplan_functions();
	sccp_unregister_cli();
	sccp_mwi_module_stop();
#ifdef CS_SCCP_MANAGER
	sccp_unregister_management();
#endif
#ifdef HAVE_PBX_MESSAGE_H
	ast_msg_tech_unregister(&sccp_msg_tech);
#endif
#ifdef CS_SCCP_CONFERENCE
	unregister_channel_tech(sccpconf_announce_get_tech());
#endif

	if (io) {
		io_context_destroy(io);
		io = NULL;
	}

	while (SCCP_REF_DESTROYED != sccp_refcount_isRunning()) {
		usleep(SCCP_TIME_TO_KEEP_REFCOUNTEDOBJECT);							// give enough time for all schedules to end and refcounted object to be cleanup completely
	}

	if (sched) {
		pbx_log(LOG_NOTICE, "Cleaning up scheduled items:\n");
		int scheduled_items = 0;

		ast_sched_dump(sched);
		while ((scheduled_items = ast_sched_runq(sched))) {
			pbx_log(LOG_NOTICE, "Cleaning up %d scheduled items... please wait\n", scheduled_items);
			usleep(ast_sched_wait(sched));
		}
		ast_sched_context_destroy(sched);
		sched = NULL;
	}

	pbx_log(LOG_NOTICE, "Running Cleanup\n");
	sccp_free(sccp_globals);
	pbx_log(LOG_NOTICE, "Module chan_sccp unloaded\n");
	return 0;
}

#if defined(__cplusplus) || defined(c_plusplus)
static ast_module_load_result load_module(void)
#else
static int load_module(void)
#endif
{
	int res = AST_MODULE_LOAD_DECLINE;
	do {
		if (ast_module_check("chan_skinny.so")) {
			pbx_log(LOG_ERROR, "Chan_skinny is loaded. Please check modules.conf and remove chan_skinny before loading chan_sccp.\n");
			break;
		}
		if (!(sched = ast_sched_context_create())) {
			pbx_log(LOG_WARNING, "Unable to create schedule context. SCCP channel type disabled\n");
			break;
		}
		if (ast_sched_start_thread(sched)) {
			pbx_log(LOG_ERROR, "Unable to start scheduler\n");
			ast_sched_context_destroy(sched);
			sched = NULL;
			break;
		}
#if defined(CS_DEVSTATE_FEATURE) || defined(CS_USE_ASTERISK_DISTRIBUTED_DEVSTATE)
		// ast_enable_distributed_devstate();
#endif
		if (!sccp_prePBXLoad()) {
			pbx_log(LOG_ERROR, "SCCP: prePBXLoad Failed\n");
			break;
		}
		if (!(io = io_context_create())) {
			pbx_log(LOG_ERROR, "Unable to create I/O context. SCCP channel type disabled\n");
			break;
		}
		if (!load_config()) {
			pbx_log(LOG_ERROR, "SCCP: config file could not be parsed\n");
			res = AST_MODULE_LOAD_DECLINE;
			break;
		}
		if (register_channel_tech(&sccp_tech)) {
			pbx_log(LOG_ERROR, "Unable to register channel class SCCP\n");
			break;
		}
#ifdef HAVE_PBX_MESSAGE_H
		if (ast_msg_tech_register(&sccp_msg_tech)) {
			pbx_log(LOG_ERROR, "Unable to register message interface\n");
			break;
		}
#endif

#ifdef CS_SCCP_CONFERENCE
		if (register_channel_tech(sccpconf_announce_get_tech())) {
			pbx_log(LOG_ERROR, "Unable to register channel class ANNOUNCE (conference)\n");
			break;
		}
#endif
		if (ast_rtp_glue_register(&sccp_rtp)) {
			pbx_log(LOG_ERROR, "Unable to register RTP Glue\n");
			break;
		}
		if (sccp_register_management()) {
			pbx_log(LOG_ERROR, "Unable to register management functions");
			break;
		}
		if (sccp_register_cli()) {
			pbx_log(LOG_ERROR, "Unable to register CLI functions");
			break;
		}
		if (sccp_register_dialplan_functions()) {
			pbx_log(LOG_ERROR, "Unable to register dialplan functions");
			break;
		}
		if (!sccp_postPBX_load()) {
			pbx_log(LOG_ERROR, "SCCP: postPBXLoad Failed\n");
			break;
		}
		res = AST_MODULE_LOAD_SUCCESS;
	} while (0);

	if (res != AST_MODULE_LOAD_SUCCESS) {
		pbx_log(LOG_ERROR, "SCCP: Module Load Failed, unloading...\n");
		unload_module();
	}
	return res;
}

static int module_reload(void)
{
	sccp_reload();
	return 0;
}

#if defined(__cplusplus) || defined(c_plusplus)

static struct ast_module_info __mod_info = {
	NULL,
	load_module,
	module_reload,
	unload_module,
	NULL,
	NULL,
	AST_MODULE,
	SCCP_VERSIONSTR,
	ASTERISK_GPL_KEY,
	AST_MODFLAG_LOAD_ORDER,
	AST_BUILDOPT_SUM,
	AST_MODPRI_CHANNEL_DRIVER,
	NULL,
};

static void __attribute__ ((constructor)) __reg_module(void)
{
	ast_module_register(&__mod_info);
}

static void __attribute__ ((destructor)) __unreg_module(void)
{
	ast_module_unregister(&__mod_info);
}

static const __attribute__ ((unused))
struct ast_module_info *ast_module_info = &__mod_info;
#else

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, SCCP_VERSIONSTR,.load = load_module,.unload = unload_module,.reload = module_reload,.load_pri = AST_MODPRI_DEFAULT,.nonoptreq = "chan_local");
#endif

PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_findPickupChannelByExtenLocked(PBX_CHANNEL_TYPE * chan, const char *exten, const char *context)
{
	struct ast_channel *target = NULL;									/*!< Potential pickup target */
	struct ast_channel_iterator *iter;

	if (!(iter = ast_channel_iterator_by_exten_new(exten, context))) {
		return NULL;
	}

	//ast_log(LOG_NOTICE, "(findPickupChannelByExtenLocked) checking pickup of channel: %s, context: %s, exten: %s, iter: %p\n", pbx_channel_name(chan), exten, context, iter);
	while ((target = ast_channel_iterator_next(iter))) {
		ast_channel_lock(target);
		//ast_log(LOG_NOTICE, "(findPickupChannelByExtenLocked) checking channel: %s, target:%s, can_pickup: %d\n", pbx_channel_name(chan), pbx_channel_name(target), ast_can_pickup(target));
		if ((chan != target) && ast_can_pickup(target)) {
			ast_log(LOG_NOTICE, "%s pickup by %s\n", ast_channel_name(target), ast_channel_name(chan));
			break;
		}
		ast_channel_unlock(target);
		target = ast_channel_unref(target);
	}

	ast_channel_iterator_destroy(iter);
	return target;
}

PBX_CHANNEL_TYPE *sccp_wrapper_asterisk115_findPickupChannelByGroupLocked(PBX_CHANNEL_TYPE * chan)
{
	struct ast_channel *target = NULL;									/*!< Potential pickup target */

	target = ast_pickup_find_by_group(chan);
	return target;
}

// kate: indent-width 8; replace-tabs off; indent-mode cstyle; auto-insert-doxygen on; line-numbers on; tab-indents on; keep-extra-spaces off; auto-brackets off;
