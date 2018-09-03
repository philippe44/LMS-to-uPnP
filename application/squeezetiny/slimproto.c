/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *	(c) Philippe 2015-2017, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 This works almost like squeezelite, but with a big difference: the STMd which
tells LMS to send the next track is only sent once the full current track has
been accepted by the player (long buffer). This makes a whole difference in term
of track boundaries management and overlap between decode and output that does
not exist at all. A decoder runs first, the output starts, then the decoder
finishes, the output finish and then, only then, another decoder can start. This
does not cause any real time issue as http players have large buffers but it
simplifies extremely buffer management. To some extend, the output buffer
pointers could be reset at the begining every time an output exit because no
decoder is running at that time
*/

/* TODO
- maybe one single CLI socket for all machines
- move from CLI to COMET
*/

#include "squeezelite.h"
#include "slimproto.h"

#define PORT 3483
#define MAXBUF 4096

#if SL_LITTLE_ENDIAN
#define LOCAL_PLAYER_IP   0x0100007f // 127.0.0.1
#define LOCAL_PLAYER_PORT 0x9b0d     // 3483
#else
#define LOCAL_PLAYER_IP   0x7f000001 // 127.0.0.1
#define LOCAL_PLAYER_PORT 0x0d9b     // 3483
#endif

extern log_level slimproto_loglevel;
static log_level *loglevel = &slimproto_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_D   mutex_lock(ctx->decode.mutex)
#define UNLOCK_D mutex_unlock(ctx->decode.mutex)
#define LOCK_P   mutex_lock(ctx->mutex)
#define UNLOCK_P mutex_unlock(ctx->mutex)

static u8_t 	pcm_sample_size[] = { 8, 16, 24, 32 };
static u32_t 	pcm_sample_rate[] = { 11025, 22050, 32000, 44100, 48000,
									  8000, 12000, 16000, 24000, 96000, 88200,
									  176400, 192000, 352800, 384000 };
static u8_t		pcm_channels[] = { 1, 2 };

static bool process_start(u8_t format, u32_t rate, u8_t size, u8_t channels,
						  u8_t endianness, struct thread_ctx_s *ctx);

/*---------------------------------------------------------------------------*/
bool ctx_callback(struct thread_ctx_s *ctx, sq_action_t action, u8_t *cookie, void *param)
{
	bool rc = false;

	if (ctx->callback) rc = ctx->callback(ctx->self, ctx->MR, action, cookie, param);
	return rc;
}

/*---------------------------------------------------------------------------*/
void send_packet(u8_t *packet, size_t len, sockfd sock) {
	u8_t *ptr = packet;
	unsigned try = 0;
	ssize_t n;

	while (len) {
		n = send(sock, ptr, len, MSG_NOSIGNAL);
		if (n <= 0) {
			int error = last_error();
#if WIN
			if (n < 0 && (error == ERROR_WOULDBLOCK || error == WSAENOTCONN) && try < 10) {
#else
			if (n < 0 && error == ERROR_WOULDBLOCK && try < 10) {
#endif
				LOG_DEBUG("retrying (%d) writing to socket", ++try);
				usleep(1000);
				continue;
			}
			LOG_WARN("failed writing to socket: %u, %s", error, strerror(last_error()));
			return;
		}
		ptr += n;
		len -= n;
	}
}

/*---------------------------------------------------------------------------*/
static void sendHELO(bool reconnect, const char *fixed_cap, const char *var_cap, u8_t mac[6], struct thread_ctx_s *ctx) {
	const char *base_cap = "Model=squeezelite,ModelName=SqueezeLite,AccuratePlayPoints=0,HasDigitalOut=1";
	struct HELO_packet pkt;

	memset(&pkt, 0, sizeof(pkt));
	memcpy(&pkt.opcode, "HELO", 4);
	pkt.length = htonl(sizeof(struct HELO_packet) - 8 + strlen(base_cap) + strlen(fixed_cap) + strlen(var_cap));
	pkt.deviceid = 12; // squeezeplay
	pkt.revision = 0;
	packn(&pkt.wlan_channellist, reconnect ? 0x4000 : 0x0000);
	packN(&pkt.bytes_received_H, (u64_t)ctx->status.stream_bytes >> 32);
	packN(&pkt.bytes_received_L, (u64_t)ctx->status.stream_bytes & 0xffffffff);
	memcpy(pkt.mac, mac, 6);

	LOG_DEBUG("[%p] mac: %02x:%02x:%02x:%02x:%02x:%02x", ctx, pkt.mac[0], pkt.mac[1], pkt.mac[2], pkt.mac[3], pkt.mac[4], pkt.mac[5]);
	LOG_INFO("[%p] cap: %s%s%s", ctx, base_cap, fixed_cap, var_cap);

	send_packet((u8_t *)&pkt, sizeof(pkt), ctx->sock);
	send_packet((u8_t *)base_cap, strlen(base_cap), ctx->sock);
	send_packet((u8_t *)fixed_cap, strlen(fixed_cap), ctx->sock);
	send_packet((u8_t *)var_cap, strlen(var_cap), ctx->sock);
}

/*---------------------------------------------------------------------------*/
static void sendSTAT(const char *event, u32_t server_timestamp, struct thread_ctx_s *ctx) {
	struct STAT_packet pkt;
	u32_t now = gettime_ms();

	memset(&pkt, 0, sizeof(struct STAT_packet));
	memcpy(&pkt.opcode, "STAT", 4);
	pkt.length = htonl(sizeof(struct STAT_packet) - 8);
	memcpy(&pkt.event, event, 4);
	// num_crlf
	// mas_initialized; mas_mode;
	packN(&pkt.stream_buffer_fullness, ctx->status.stream_full);
	packN(&pkt.stream_buffer_size, ctx->status.stream_size);
	packN(&pkt.bytes_received_H, (u64_t)ctx->status.stream_bytes >> 32);
	packN(&pkt.bytes_received_L, (u64_t)ctx->status.stream_bytes & 0xffffffff);
	pkt.signal_strength = 0xffff;
	packN(&pkt.jiffies, now);
	packN(&pkt.output_buffer_size, ctx->status.output_size);
	packN(&pkt.output_buffer_fullness, ctx->status.output_full);
	packN(&pkt.elapsed_seconds, ctx->status.ms_played / 1000);
	// voltage;
	packN(&pkt.elapsed_milliseconds, ctx->status.ms_played);
	pkt.server_timestamp = server_timestamp; // keep this is server format - don't unpack/pack

	if (strcmp(event, "STMt") || *loglevel == lDEBUG) {
		LOG_INFO("[%p]: STAT:[%s] msplayed %d", ctx, event, ctx->status.ms_played);
	}

	send_packet((u8_t *)&pkt, sizeof(pkt), ctx->sock);
}

/*---------------------------------------------------------------------------*/
static void sendDSCO(disconnect_code disconnect, sockfd sock) {
	struct DSCO_packet pkt;

	memset(&pkt, 0, sizeof(pkt));
	memcpy(&pkt.opcode, "DSCO", 4);
	pkt.length = htonl(sizeof(pkt) - 8);
	pkt.reason = disconnect & 0xFF;

	LOG_DEBUG("[%d]: DSCO: %d", sock, disconnect);

	send_packet((u8_t *)&pkt, sizeof(pkt), sock);
}

/*---------------------------------------------------------------------------*/
static void sendRESP(const char *header, size_t len, sockfd sock) {
	struct RESP_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "RESP", 4);
	pkt_header.length = htonl(sizeof(pkt_header) + len - 8);

	LOG_DEBUG("[%d]: RESP", sock);

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header), sock);
	send_packet((u8_t *)header, len, sock);
}

/*---------------------------------------------------------------------------*/
static void sendMETA(const char *meta, size_t len, sockfd sock) {
	struct META_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "META", 4);
	pkt_header.length = htonl(sizeof(pkt_header) + len - 8);

	LOG_DEBUG("[%d]: META", sock);

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header), sock);
	send_packet((u8_t *)meta, len, sock);
}

/*---------------------------------------------------------------------------*/
static void sendSETDName(const char *name, sockfd sock) {
	struct SETD_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "SETD", 4);

	pkt_header.id = 0; // id 0 is playername S:P:Squeezebox2
	pkt_header.length = htonl(sizeof(pkt_header) + strlen(name) + 1 - 8);

	LOG_DEBUG("[%d]: set playername: %s", sock, name);

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header), sock);
	send_packet((u8_t *)name, strlen(name) + 1, sock);
}

/*---------------------------------------------------------------------------*/
static void process_strm(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct strm_packet *strm = (struct strm_packet *)pkt;

	if (strm->command != 't' && strm->command != 'q') {
		LOG_INFO("[%p] strm command %c", ctx, strm->command);
	}
	else {
		LOG_DEBUG("[%p] strm command %c", ctx, strm->command);
	}

	switch(strm->command) {
	case 't':
		sendSTAT("STMt", strm->replay_gain, ctx); // STMt replay_gain is no longer used to track latency, but support it
		break;
	case 'f':
		decode_flush(ctx);
		output_flush(ctx);
		stream_disconnect(ctx);
		ctx->status.ms_played = 0;
		sendSTAT("STMf", 0, ctx);
		buf_flush(ctx->streambuf);
		break;
	case 'q':
		decode_flush(ctx);
		output_flush(ctx);
		ctx->status.ms_played = 0;
		if (stream_disconnect(ctx))
			sendSTAT("STMf", 0, ctx);
		buf_flush(ctx->streambuf);
		if (ctx->last_command != 'q') ctx_callback(ctx, SQ_STOP, NULL, NULL);
		break;
	case 'p':
		{
			unsigned interval = unpackN(&strm->replay_gain);
			LOG_INFO("[%p] pause (interval:%u)", ctx, interval);
			if (!interval) {
				LOCK_O;
				ctx->output.state = OUTPUT_WAITING;
				UNLOCK_O;
				ctx_callback(ctx, SQ_PAUSE, NULL, NULL);
				sendSTAT("STMp", 0, ctx);
			}
		}
		break;
	case 'a':
		{
			unsigned interval = unpackN(&strm->replay_gain);
			LOG_INFO("[%p]skip ahead interval (ignored): %u", ctx, interval);
		}
		break;
	case 'u':
		{
			unsigned jiffies = unpackN(&strm->replay_gain);
			LOG_INFO("[%p] unpause at: %u now: %u", ctx, jiffies, gettime_ms());
			ctx_callback(ctx, SQ_UNPAUSE, NULL, NULL);
			LOCK_O;
			ctx->output.state = OUTPUT_RUNNING;
			ctx->output.start_at = jiffies;
			UNLOCK_O;
			sendSTAT("STMr", 0, ctx);
		}
		break;
	case 's':
		{
			bool sendSTMn = false;
			unsigned header_len = len - sizeof(struct strm_packet);
			char *header = (char *)(pkt + sizeof(struct strm_packet));
			in_addr_t ip = (in_addr_t)strm->server_ip; // keep in network byte order
			u16_t port = strm->server_port; // keep in network byte order

			if (ip == 0) ip = ctx->slimproto_ip;

			LOG_INFO("[%p], strm s autostart: %c transition period: %u transition type: %u codec: %c",
					  ctx, strm->autostart, strm->transition_period, strm->transition_type - '0', strm->format);

			ctx->autostart = strm->autostart - '0';

			sendSTAT("STMf", 0, ctx);

			if (header_len > MAX_HEADER -1) {
				LOG_WARN("[%p] header too long: %u", ctx, header_len);
				break;
			}

			ctx->output.next_replay_gain = unpackN(&strm->replay_gain);
			ctx->output.fade_mode = strm->transition_type - '0';
			ctx->output.fade_secs = strm->transition_period;

			LOG_DEBUG("[%p]: set fade mode: %u", ctx, ctx->output.fade_mode);

			if (strm->format != '?') {
				sendSTMn = !process_start(strm->format, strm->pcm_sample_rate, strm->pcm_sample_size,
										  strm->pcm_channels, strm->pcm_endianness, ctx);
			} else if (ctx->autostart >= 2) {
				// extension to slimproto to allow server to detect codec from response header and send back in codc message
				LOG_INFO("[%p] waiting for codc message", ctx);
			} else {
				LOG_ERROR("[%p] unknown codec requires autostart >= 2", ctx);
				break;
			}

			stream_sock(ip, port, header, header_len, strm->threshold * 1024, ctx->autostart >= 2, ctx);

			sendSTAT("STMc", 0, ctx);
			ctx->canSTMdu = ctx->sentSTMu = ctx->sentSTMo = ctx->sentSTMl = ctx->sentSTMd = false;

			// codec error
			if (sendSTMn) {
				LOG_ERROR("[%p] no matching codec %c", ctx, ctx->output.codec);
				sendSTAT("STMn", 0, ctx);
			}
		}
		break;
	default:
		LOG_WARN("[%p] unhandled strm %c", ctx, strm->command);
		break;
	}

	ctx->last_command = strm->command;
}

/*---------------------------------------------------------------------------*/
static void process_cont(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct cont_packet *cont = (struct cont_packet *)pkt;

	cont->metaint = unpackN(&cont->metaint);

	LOG_DEBUG("cont metaint: %u loop: %u", cont->metaint, cont->loop);

	if (ctx->autostart > 1) {
		ctx->autostart -= 2;
		LOCK_S;
		if (ctx->stream.state == STREAMING_WAIT) {
			ctx->stream.state = STREAMING_BUFFERING;
			ctx->stream.meta_interval = ctx->stream.meta_next = cont->metaint;
		}
		UNLOCK_S;
		wake_controller(ctx);
	}
}

/*---------------------------------------------------------------------------*/
static void process_codc(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct codc_packet *codc = (struct codc_packet *)pkt;

	if (!process_start(codc->format, codc->pcm_sample_rate, codc->pcm_sample_size,
					  codc->pcm_channels, codc->pcm_endianness, ctx)) {
		LOG_ERROR("[%p] codc error %c", ctx);
		sendSTAT("STMn", 0, ctx);
	}
}

/*---------------------------------------------------------------------------*/
static void process_aude(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct aude_packet *aude = (struct aude_packet *)pkt;

	LOCK_O;
	ctx->on = (aude->enable_spdif) ? true : false;
	LOG_DEBUG("[%p] on/off using aude %d", ctx, ctx->on);
	UNLOCK_O;

	ctx_callback(ctx, SQ_ONOFF, NULL, &ctx->on);
}

/*---------------------------------------------------------------------------*/
static void process_audg(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct audg_packet *audg = (struct audg_packet *)pkt;
	u16_t  gain;

	audg->old_gainL = unpackN(&audg->old_gainL);
	audg->old_gainR = unpackN(&audg->old_gainR);

	LOG_DEBUG("[%p] (old) audg gainL: %u gainR: %u", ctx, audg->old_gainL, audg->old_gainR);

	gain = (audg->old_gainL + audg->old_gainL) / 2;
	if (audg->adjust) {
		ctx_callback(ctx, SQ_VOLUME, NULL, (void*) &gain);
    }
}

/*---------------------------------------------------------------------------*/
static void process_setd(u8_t *pkt, int len,struct thread_ctx_s *ctx) {
	struct setd_packet *setd = (struct setd_packet *)pkt;

	// handle player name query and change
	if (setd->id == 0) {
		if (len == 5) {
			if (strlen(ctx->config.name)) {
				sendSETDName(ctx->config.name, ctx->sock);
			}
		} else if (len > 5) {
			strncpy(ctx->config.name, setd->data, _STR_LEN_);
			ctx->config.name[_STR_LEN_ - 1] = '\0';
			LOG_DEBUG("[%p] set name: %s", ctx, setd->data);
			// confirm change to server
			sendSETDName(setd->data, ctx->sock);
			ctx_callback(ctx, SQ_SETNAME, NULL, (void*) ctx->config.name);
		}
	}
}

/*---------------------------------------------------------------------------*/
static void process_ledc(u8_t *pkt, int len,struct thread_ctx_s *ctx) {
	LOG_DEBUG("[%p] ledc", ctx);
}


#define SYNC_CAP ",SyncgroupID="
#define SYNC_CAP_LEN 13

/*---------------------------------------------------------------------------*/
static void process_serv(u8_t *pkt, int len,struct thread_ctx_s *ctx) {
	struct serv_packet *serv = (struct serv_packet *)pkt;

	LOG_INFO("[%p] switch server", ctx);

	ctx->new_server = serv->server_ip;

	if (len - sizeof(struct serv_packet) == 10) {
		if (!ctx->new_server_cap) {
			ctx->new_server_cap = malloc(SYNC_CAP_LEN + 10 + 1);
		}
		ctx->new_server_cap[0] = '\0';
		strcat(ctx->new_server_cap, SYNC_CAP);
		strncat(ctx->new_server_cap, (const char *)(pkt + sizeof(struct serv_packet)), 10);
	} else {
		if (ctx->new_server_cap) {
			free(ctx->new_server_cap);
			ctx->new_server_cap = NULL;
		}
	}

	ctx_callback(ctx, SQ_SETSERVER, NULL, (void*) &ctx->new_server);
}

/*---------------------------------------------------------------------------*/
static void process_vers(u8_t *pkt, int len,struct thread_ctx_s *ctx) {
	struct vers_packet *vers = (struct vers_packet *)pkt;

	LOG_DEBUG("[%p] version %s", ctx, vers->version);
}

struct handler {
	char opcode[6];
	void (*handler)(u8_t *, int, struct thread_ctx_s *);
};

static struct handler handlers[] = {
	{ "strm", process_strm },
	{ "cont", process_cont },
	{ "codc", process_codc },
	{ "aude", process_aude },
	{ "audg", process_audg },
	{ "setd", process_setd },
	{ "serv", process_serv },
	{ "ledc", process_ledc },
	{ "vers", process_vers },
	{ "",     NULL  },
};

/*---------------------------------------------------------------------------*/
static void process(u8_t *pack, int len, struct thread_ctx_s *ctx) {
	struct handler *h = handlers;
	while (h->handler && strncmp((char *)pack, h->opcode, 4)) { h++; }

	if (h->handler) {
		LOG_DEBUG("[%p] %s", ctx, h->opcode);
		h->handler(pack, len, ctx);
	} else {
		pack[4] = '\0';
		LOG_WARN("[%p] unhandled %s", ctx, (char *)pack);
	}
}

/*---------------------------------------------------------------------------*/
static void slimproto_run(struct thread_ctx_s *ctx) {
	int  expect = 0;
	int  got    = 0;
	u32_t now;
	event_handle ehandles[2];
	int timeouts = 0;

	set_readwake_handles(ehandles, ctx->sock, ctx->wake_e);

	while (ctx->running && !ctx->new_server) {

		bool wake = false;
		event_type ev;

		if ((ev = wait_readwake(ehandles, 1000)) != EVENT_TIMEOUT) {

			if (ev == EVENT_READ) {

				if (expect > 0) {
					int n = recv(ctx->sock, ctx->slim_run.buffer + got, expect, 0);
					if (n <= 0) {
						if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
							continue;
						}
						LOG_WARN("[%p] error reading from socket: %s", ctx, n ? strerror(last_error()) : "closed");
						return;
					}
					expect -= n;
					got += n;
					if (expect == 0) {
						process(ctx->slim_run.buffer, got, ctx);
						got = 0;
					}
				} else if (expect == 0) {
					int n = recv(ctx->sock, ctx->slim_run.buffer + got, 2 - got, 0);
					if (n <= 0) {
						if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
							continue;
						}
						LOG_WARN("[%p] error reading from socket: %s", ctx, n ? strerror(last_error()) : "closed");
						return;
					}
					got += n;
					if (got == 2) {
						expect = ctx->slim_run.buffer[0] << 8 | ctx->slim_run.buffer[1]; // length pack 'n'
						got = 0;
						if (expect > MAXBUF) {
							LOG_ERROR("[%p] FATAL: slimproto packet too big: %d > %d", ctx, expect, MAXBUF);
							return;
						}
					}
				} else {
					LOG_ERROR("[%p] FATAL: negative expect", ctx);
					return;
				}

			}

			if (ev == EVENT_WAKE) {
				wake = true;
			}

			if (ctx->cli_sock > 0 && ((ctx->cli_timestamp + 10000) - gettime_ms() > 10000)) {
				if (!mutex_trylock(ctx->cli_mutex)) {
					LOG_INFO("[%p] Closing CLI socket %d", ctx, ctx->cli_sock);
					closesocket(ctx->cli_sock);
					ctx->cli_sock = -1;
					mutex_unlock(ctx->cli_mutex);
				}
			}

			timeouts = 0;

		} else if (++timeouts > 35) {

			// expect message from server every 5 seconds, but 30 seconds on mysb.com so timeout after 35 seconds
			LOG_WARN("[%p] No messages from server - connection dead", ctx);
			return;
		}

		// update playback state when woken or every 100ms
		now = gettime_ms();

		// check for metadata update (LOCK_O not really necessary here)
		if (ctx->output.state == OUTPUT_RUNNING && ctx->config.send_icy && 
			ctx->output.icy.interval && (ctx->output.icy.last + ICY_UPDATE_TIME) - now > ICY_UPDATE_TIME) {
			struct metadata_s metadata;

			sq_get_metadata(ctx->self, &metadata, 0);
			output_set_icy(&metadata, false, now, ctx);
			sq_free_metadata(&metadata);
		}

		if (wake || now - ctx->slim_run.last > 100 || ctx->slim_run.last > now) {
			bool _sendSTMs = false;
			bool _sendDSCO = false;
			bool _sendRESP = false;
			bool _sendMETA = false;
			bool _sendSTMd = false;
			bool _sendSTMt = false;
			bool _sendSTMl = false;
			bool _sendSTMu = false;
			bool _sendSTMo = false;
			bool _sendSTMn = false;
			bool _stream_disconnect = false;
			disconnect_code disconnect_code;
			size_t header_len = 0;

			ctx->slim_run.last = now;

			LOCK_S;

			ctx->status.stream_full = _buf_used(ctx->streambuf);
			ctx->status.stream_size = ctx->streambuf->size;
			ctx->status.stream_bytes = ctx->stream.bytes;
			ctx->status.stream_state = ctx->stream.state;

			if (ctx->stream.state == DISCONNECT) {
				disconnect_code = ctx->stream.disconnect;
				ctx->stream.state = STOPPED;
				_sendDSCO = true;
			}

			if (!ctx->stream.sent_headers &&
				(ctx->stream.state == STREAMING_HTTP || ctx->stream.state == STREAMING_WAIT ||
				 ctx->stream.state == STREAMING_BUFFERING)) {
				header_len = ctx->stream.header_len;
				memcpy(ctx->slim_run.header, ctx->stream.header, header_len);
				_sendRESP = true;
				ctx->stream.sent_headers = true;
			}
			if (ctx->stream.meta_send) {
				header_len = ctx->stream.header_len;
				memcpy(ctx->slim_run.header, ctx->stream.header, header_len);
				_sendMETA = true;
				ctx->stream.meta_send = false;
			}

			UNLOCK_S;

			LOCK_O;
			ctx->status.output_full = ctx->sentSTMu ? 0 : ctx->outputbuf->size / 2;
			ctx->status.output_size = ctx->outputbuf->size;
			ctx->status.sample_rate = ctx->output.sample_rate;
			ctx->status.output_ready = ctx->output.completed || ctx->output.encode.flow;
			ctx->status.duration = ctx->render.duration;
			ctx->status.ms_played = ctx->render.ms_played;

			// streaming properly started
			if (ctx->output.track_started) {
				_sendSTMs = true;
				ctx->canSTMdu = true;
				ctx->output.track_started = false;
			}

			// streaming failed, wait till output thread ends and move on
			if (ctx->status.stream_bytes == 0 && ctx->output.completed && ctx->output.state == OUTPUT_RUNNING) {
				LOG_WARN("[%p]: nothing received", ctx);
				// when streaming fails, need to make sure we move on
				ctx->render.state = RD_STOPPED;
				ctx->canSTMdu = true;
				_sendSTMn = true;
			}

			// normal end of track with underrun
			if (ctx->output.state == OUTPUT_RUNNING && !ctx->sentSTMu &&
				ctx->status.output_ready && ctx->status.stream_state <= DISCONNECT &&
				ctx->render.state == RD_STOPPED && ctx->canSTMdu) {
				_sendSTMu = true;
				ctx->sentSTMu = true;
				ctx->status.output_full = 0;
				ctx->output.encode.flow = false;
				ctx->output.state = OUTPUT_STOPPED;
			}

			// if there is still data to be sent, try an overrun
			if (ctx->output.state == OUTPUT_RUNNING && !ctx->sentSTMo &&
				ctx->status.stream_state == STREAMING_HTTP &&
				ctx->render.state == RD_STOPPED && ctx->canSTMdu) {
				_sendSTMo = true;
				ctx->sentSTMo = true;
				ctx->output.state = OUTPUT_STOPPED;
			}

			UNLOCK_O;

			LOCK_D;

			if (ctx->decode.state == DECODE_RUNNING && now - ctx->status.last > 1000) {
				_sendSTMt = true;
				ctx->status.last = now;
			}

			if ((ctx->status.stream_state == STREAMING_HTTP || ctx->status.stream_state == STREAMING_FILE ||
				(ctx->status.stream_state == DISCONNECT && ctx->stream.disconnect == DISCONNECT_OK)) &&
				!ctx->sentSTMl && ctx->decode.state == DECODE_READY) {
				if (ctx->autostart == 0) {
					ctx->decode.state = DECODE_RUNNING;
					_sendSTMl = true;
					ctx->sentSTMl = true;
				} else if (ctx->autostart == 1) {
					ctx->decode.state = DECODE_RUNNING;
					LOCK_O;
					// release output thread now that we are decoding
					ctx->output.state = OUTPUT_RUNNING;
					UNLOCK_O;
				}
				ctx_callback(ctx, SQ_PLAY, NULL, NULL);
				// autostart 2 and 3 require cont to be received first
			}

			/*
			 Unless flow mode is used, wait for all output to be sent to the
			 player before asking for next track. The outputbuf must be empty
			 and STMs sent, because for short tracks the output thread might
			 exit before playback has started and we don't want to send STMd
			 before STMs.
			 Streaming services like Deezer or RP plugin close connection if
			 stalled for too long (30s), so if STMd is sent too early, once the
			 outputbuf is filled, connection will be idle for a while, so need
			 to wait a bit toward the end of the track before sending STMd.
			 But when flow mode is used, the stream is regulated by the player
			 and thus should be continuous, so there is no need to wait toward
			 the end of the track, which also fits nicely with the requirement
			 for cross fade (need to have enough of current track in outputbuf
			 when codec of next track starts)
			*/
			if ((ctx->decode.state == DECODE_COMPLETE && ctx->canSTMdu && ctx->status.output_ready &&
				(ctx->output.encode.flow || !ctx->output.remote ||
				 (ctx->status.duration && ctx->status.duration - ctx->status.ms_played < STREAM_DELAY))) ||
				ctx->decode.state == DECODE_ERROR) {

				if (ctx->decode.state == DECODE_COMPLETE) _sendSTMd = true;
				if (ctx->decode.state == DECODE_ERROR)    _sendSTMn = true;
				ctx->decode.state = DECODE_STOPPED;
				if (ctx->status.stream_state == STREAMING_HTTP ||
					ctx->status.stream_state == STREAMING_FILE) {
					_stream_disconnect = true;
				}

			}

			UNLOCK_D;

			if (_stream_disconnect) stream_disconnect(ctx);

			// send packets once locks released as packet sending can block
			if (_sendDSCO) sendDSCO(disconnect_code, ctx->sock);
			if (_sendSTMs) sendSTAT("STMs", 0, ctx);
			if (_sendSTMt) sendSTAT("STMt", 0, ctx);
			if (_sendSTMl) sendSTAT("STMl", 0, ctx);
			if (_sendSTMd) sendSTAT("STMd", 0, ctx);
			if (_sendSTMu) sendSTAT("STMu", 0, ctx);
			if (_sendSTMo) sendSTAT("STMo", 0, ctx);
			if (_sendSTMn) sendSTAT("STMn", 0, ctx);
			if (_sendRESP) sendRESP(ctx->slim_run.header, header_len, ctx->sock);
			if (_sendMETA) sendMETA(ctx->slim_run.header, header_len, ctx->sock);
		}
	}
}

 /*---------------------------------------------------------------------------*/
// called from other threads to wake state machine above
void wake_controller(struct thread_ctx_s *ctx) {
	wake_signal(ctx->wake_e);
}

void discover_server(struct thread_ctx_s *ctx) {
	struct sockaddr_in d;
	struct sockaddr_in s;
	char buf[32], vers[] = "VERS", port[] = "JSON", clip[] = "CLIP";
	struct pollfd pollinfo;
	u8_t len;
	int disc_sock = socket(AF_INET, SOCK_DGRAM, 0);
	socklen_t enable = 1;

	ctx->cli_port = 9090;

	setsockopt(disc_sock, SOL_SOCKET, SO_BROADCAST, (const void *)&enable, sizeof(enable));

	len = sprintf(buf,"e%s%c%s%c%s", vers, '\0', port, '\0', clip) + 1;

	memset(&d, 0, sizeof(d));
	d.sin_family = AF_INET;
	d.sin_port = htons(PORT);
	if (!ctx->slimproto_ip) d.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	else d.sin_addr.s_addr = ctx->slimproto_ip;

	pollinfo.fd = disc_sock;
	pollinfo.events = POLLIN;

	do {
		LOG_DEBUG("[%p] sending discovery", ctx);
		memset(&s, 0, sizeof(s));

		if (sendto(disc_sock, buf, len, 0, (struct sockaddr *)&d, sizeof(d)) < 0) {
			LOG_WARN("[%p] error sending discovery", ctx);
		}

		if (poll(&pollinfo, 1, 5000) == 1) {
			char readbuf[32], *p;

			socklen_t slen = sizeof(s);
			memset(readbuf, 0, 32);
			recvfrom(disc_sock, readbuf, 32 - 1, 0, (struct sockaddr *)&s, &slen);

			if ((p = strstr(readbuf, vers)) != NULL) {
				p += strlen(vers);
				strncpy(ctx->server_version, p + 1, min(SERVER_VERSION_LEN, *p));
				ctx->server_version[min(SERVER_VERSION_LEN, *p)] = '\0';
			}

			 if ((p = strstr(readbuf, port)) != NULL) {
				p += strlen(port);
				strncpy(ctx->server_port, p + 1, min(5, *p));
				ctx->server_port[min(6, *p)] = '\0';
			}

			 if ((p = strstr(readbuf, clip)) != NULL) {
				p += strlen(clip);
				ctx->cli_port = atoi(p + 1);
			}

			strcpy(ctx->server_ip, inet_ntoa(s.sin_addr));
			LOG_DEBUG("[%p] got response from: %s:%d", ctx, inet_ntoa(s.sin_addr), ntohs(s.sin_port));
		}
	} while (s.sin_addr.s_addr == 0 && ctx->running);

	closesocket(disc_sock);

	ctx->slimproto_ip =  s.sin_addr.s_addr;
	ctx->slimproto_port = ntohs(s.sin_port);

	ctx->serv_addr.sin_port = s.sin_port;
	ctx->serv_addr.sin_addr.s_addr = s.sin_addr.s_addr;
	ctx->serv_addr.sin_family = AF_INET;
}

/*---------------------------------------------------------------------------*/
static void slimproto(struct thread_ctx_s *ctx) {
	bool reconnect = false;
	unsigned failed_connect = 0;

	mutex_create(ctx->mutex);
	mutex_create(ctx->cli_mutex);

	discover_server(ctx);
	LOG_INFO("squeezelite [%p] <=> player [%p]", ctx, ctx->MR);
	LOG_INFO("[%p] connecting to %s:%d", ctx, inet_ntoa(ctx->serv_addr.sin_addr), ntohs(ctx->serv_addr.sin_port));

	while (ctx->running) {

		if (ctx->new_server) {
			ctx->slimproto_ip = ctx->new_server;
			ctx->new_server = 0;
			reconnect = false;

			discover_server(ctx);
			LOG_INFO("[%p] switching server to %s:%d", ctx, inet_ntoa(ctx->serv_addr.sin_addr), ntohs(ctx->serv_addr.sin_port));
		}

		ctx->cli_sock = -1;
		ctx->sock = socket(AF_INET, SOCK_STREAM, 0);
		set_nonblock(ctx->sock);
		set_nosigpipe(ctx->sock);

		if (connect_timeout(ctx->sock, (struct sockaddr *) &ctx->serv_addr, sizeof(ctx->serv_addr), 5*1000) != 0) {

			LOG_WARN("[%p] unable to connect to server %u", ctx, failed_connect);
			sleep(5);

			// rediscover server if it was not set at startup
			if (!strcmp(ctx->config.server, "?") && ++failed_connect > 5) {
				ctx->slimproto_ip = 0;
				discover_server(ctx);
			}

		} else {

			LOG_INFO("[%p] connected", ctx);

			ctx->var_cap[0] = '\0';
			failed_connect = 0;

			// add on any capablity to be sent to the new server
			if (ctx->new_server_cap) {
				strcat(ctx->var_cap, ctx->new_server_cap);
				free(ctx->new_server_cap);
				ctx->new_server_cap = NULL;
			}

			sendHELO(reconnect, ctx->fixed_cap, ctx->var_cap, ctx->mac, ctx);

			slimproto_run(ctx);

			if (!reconnect) {
				reconnect = true;
			}

			usleep(100000);
		}

		closesocket(ctx->sock);
		if (ctx->cli_sock != -1) closesocket(ctx->cli_sock);
		if (ctx->new_server_cap)	{
			free(ctx->new_server_cap);
			ctx->new_server_cap = NULL;
		}
	}

	mutex_destroy(ctx->mutex);
	mutex_destroy(ctx->cli_mutex);
}


/*---------------------------------------------------------------------------*/
void slimproto_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p] slimproto stop for %s", ctx, ctx->config.name);
  	ctx->running = false;
	wake_controller(ctx);
	pthread_join(ctx->thread, NULL);
}


/*---------------------------------------------------------------------------*/
void slimproto_thread_init(struct thread_ctx_s *ctx) {
	char _codecs[_STR_LEN_] = "";

	wake_create(ctx->wake_e);

	ctx->running = true;
	ctx->slimproto_ip = 0;
	ctx->slimproto_port = PORT;
	ctx->sock = -1;

	if (strcmp(ctx->config.server, "?")) {
		server_addr(ctx->config.server, &ctx->slimproto_ip, &ctx->slimproto_port);
	}

	/* could be avoided as whole context is reset at init ...*/
	strcpy(ctx->var_cap, "");
	ctx->new_server_cap = NULL;
	ctx->new_server = 0;

	// only use successfully loaded codecs in full processing mode
	if (!stristr(ctx->config.mode, "thru")) {
		char item[4], *p = ctx->config.codecs;
		int i;

		while (p && sscanf(p, "%3[^,]", item) > 0) {
			for (i = 0; i < MAX_CODECS; i++) if (codecs[i] && stristr(codecs[i]->types, item)) {
				if (*_codecs) strcat(_codecs, ",");
				strcat(_codecs, item);
				break;
			}
			p = strchr(p, ',');
			if (p) p++;
		}
	} else strcpy(_codecs, ctx->config.codecs);

	sprintf(ctx->fixed_cap, ",MaxSampleRate=%u,%s", ctx->config.sample_rate, _codecs);

	memcpy(ctx->mac, ctx->config.mac, 6);

	pthread_create(&ctx->thread, NULL, (void *(*)(void*)) slimproto, ctx);
}

/*---------------------------------------------------------------------------*/
static bool process_start(u8_t format, u32_t rate, u8_t size, u8_t channels, u8_t endianness,
						  struct thread_ctx_s *ctx) {
	struct outputstate *out = &ctx->output;
	struct track_param info;
	char *mimetype = NULL, *p, *mode = ctx->config.mode;
	bool ret = false;
	s32_t sample_rate;

	LOCK_O;
	out->index++;
	// try to handle next track failed stream where we jump over N tracks
	info.offset = ctx->render.index != -1 ? out->index - ctx->render.index : 0;
	_buf_resize(ctx->outputbuf, ctx->config.outputbuf_size);
	UNLOCK_O;

	/*
	No further LOCK_O used because tehre is either no output thread active or it
	is in draining mode (or flow) and then does not do concurrent access to the
	output context
	*/

	// get metadata - they must be freed by callee whenever he wants
	sq_get_metadata(ctx->self, &info.metadata, info.offset);

	// set key parameters
	out->completed = false;
	out->duration = info.metadata.duration;
	out->bitrate = info.metadata.bitrate;
	out->remote = info.metadata.remote;

	// read source parameters (if any)
	if (format != 'a')
		out->sample_size = (size != '?') ? pcm_sample_size[size - '0'] : 0;
	else
		out->sample_size = size;
	out->sample_rate = (rate != '?') ? pcm_sample_rate[rate - '0'] : 0;
	if (ctx->output.sample_rate > ctx->config.sample_rate) {
		 LOG_WARN("[%p]: Sample rate %u error suspected, forcing to %u", ctx, out->sample_rate, ctx->config.sample_rate);
		 out->sample_rate = ctx->config.sample_rate;
	}
	out->channels = (channels != '?') ? pcm_channels[channels - '1'] : 0;
	out->in_endian = (endianness != '?') ? endianness - '0' : 0xff;
	out->codec = format;

	// in flow mode we now have eveything, just initialize codec
	if (out->encode.flow) {
		sq_free_metadata(&info.metadata);
		return codec_open(out->codec, out->sample_size, out->sample_rate,
						  out->channels, out->in_endian, ctx);
	}

	// detect processing mode
	if (stristr(mode, "pcm")) out->encode.mode = ENCODE_PCM;
	else if (stristr(mode, "flc")) out->encode.mode = ENCODE_FLAC;
	else if (stristr(mode, "mp3")) out->encode.mode = ENCODE_MP3;
	else {
		// make sure we have a stable default mode
		strcpy(mode, "thru");
		out->encode.mode = ENCODE_THRU;
	}	

	// force read of re-encoding parameters
	if ((p = stristr(mode, "r:")) != NULL) sample_rate = atoi(p+2);
	else sample_rate = 0;
	if ((p = stristr(mode, "s:")) != NULL) out->encode.sample_size = atoi(p+2);
	else out->encode.sample_size = 0;

	// force re-encoding channels to be re-read
	out->encode.channels = 0;
	// reset time offset for new tracks
	out->offset = 0;
	// set ICY metadata if possible
	if (ctx->config.send_icy && (!out->duration || out->encode.flow))
		output_set_icy(&info.metadata, true, gettime_ms(), ctx);

	// in case of flow, all parameters shall be set
	if (stristr(mode, "flow") && out->encode.mode != ENCODE_THRU) {
		if (ctx->config.send_icy) output_set_icy(&info.metadata, true, gettime_ms(), ctx);
		sq_free_metadata(&info.metadata);
		sq_default_metadata(&info.metadata, true);

		if (!sample_rate || sample_rate < 0) sample_rate = 44100;
		if (!out->encode.sample_size) out->encode.sample_size = 16;
		out->encode.channels = 2;
		out->encode.flow = true;
	} else {
		if (ctx->config.send_icy && !out->duration) output_set_icy(&info.metadata, true, gettime_ms(), ctx);
	}

	// set sample rate for re-encoding
	if (sample_rate > 0) out->supported_rates[0] = sample_rate;
	else if (sample_rate < 0) out->supported_rates[0] = out->sample_rate ? min(out->sample_rate, abs(sample_rate)) : sample_rate;
	else out->supported_rates[0] = out->sample_rate;

	if (out->supported_rates[0] > 0) out->encode.sample_rate = out->supported_rates[0];
	else out->encode.sample_rate = 0;

	// check if re-encoding is needed
	if (out->encode.mode == ENCODE_THRU || (out->encode.mode == ENCODE_PCM && out->codec == 'p')) {

		// pcm needs alignment which is not guaranteed in THRU mode
		if (out->encode.mode == ENCODE_THRU && !_buf_reset(ctx->outputbuf)) {
			LOG_ERROR("[%p]: buffer should be empty", ctx);
		}

		if (out->codec == 'p') {
			if (!out->encode.sample_size)
				out->encode.sample_size = (out->sample_size == 24 && ctx->config.L24_format == L24_TRUNC16) ? 16 : out->sample_size;

			mimetype = find_pcm_mimetype(&out->encode.sample_size, ctx->config.L24_format == L24_TRUNC16_PCM,
										 out->encode.sample_rate, out->channels,
										 ctx->mimetypes, ctx->config.raw_audio_format);
			out->encode.mode = ENCODE_PCM;
		} else {
			mimetype = find_mimetype(out->codec, ctx->mimetypes, NULL);
			if (out->codec == 'f') out->codec ='c';
			else out->codec = '*';
		}

	} else if (out->encode.mode == ENCODE_PCM) {

		if (out->encode.sample_rate && out->encode.sample_size) {
			// everything is fixed
			mimetype = find_pcm_mimetype(&out->encode.sample_size, ctx->config.L24_format == L24_TRUNC16_PCM,
										 out->encode.sample_rate, 2, ctx->mimetypes, ctx->config.raw_audio_format);
		} else if ((info.metadata.sample_size || out->encode.sample_size) &&
				   (info.metadata.sample_rate || out->encode.sample_rate || out->supported_rates[0])) {
			u8_t sample_size = out->encode.sample_size ? out->encode.sample_size : info.metadata.sample_size;
			u32_t sample_rate;

			// try to use source format, but return generic mimetype
			if (out->encode.sample_rate) sample_rate = out->encode.sample_rate;
			else if (out->supported_rates[0] < 0) sample_rate = abs(out->supported_rates[0]);
			else sample_rate = info.metadata.sample_rate;

			mimetype = find_pcm_mimetype(&sample_size, ctx->config.L24_format == L24_TRUNC16_PCM,
										   sample_rate, 2, ctx->mimetypes, ctx->config.raw_audio_format);

			 // if matching found, set generic format "*" if audio/L used
			 if (strstr(mimetype, "audio/L")) strcpy(mimetype, "*");
		} else {
			char format[16] = "";

			// really can't use raw format
			if (stristr(ctx->config.raw_audio_format, "wav")) strcat(format, "wav");
			if (stristr(ctx->config.raw_audio_format, "aif")) strcat(format, "aif");

			mimetype = find_mimetype('p', ctx->mimetypes, format);
		}
		
	} else if (out->encode.mode == ENCODE_FLAC) {

		mimetype = find_mimetype('f', ctx->mimetypes, NULL);
		if (out->sample_size > 24) out->encode.sample_size = 24;
		if ((p = stristr(mode, "flac:")) != NULL) out->encode.level = atoi(p+5);
		if (out->encode.level > 9) out->encode.level = 0;

	} else if (out->encode.mode == ENCODE_MP3) {

		mimetype = find_mimetype('m', ctx->mimetypes, NULL);
		out->encode.sample_size = 16;
		// need to tweak a bit samples rates
		if (!out->supported_rates[0] || out->supported_rates[0] < -48000 ) out->supported_rates[0] = -48000;
		else if (out->supported_rates[0] > 48000) out->supported_rates[0] = out->encode.sample_rate = 48000;
		if ((p = stristr(mode, "mp3:")) != NULL) {
			out->encode.level = atoi(p+4);
			if (out->encode.level > 320) out->encode.level = 320;
		} else out->encode.level = 128;
	}
	// matching found in player
	if (mimetype) {
		strcpy(out->mimetype, mimetype);
		free(mimetype);

		out->format = mimetype2format(out->mimetype);
		out->out_endian = (out->format == 'w');
		out->length = ctx->config.stream_length;

		if (codec_open(out->codec, out->sample_size, out->sample_rate, out->channels,
			out->in_endian, ctx) &&	output_start(ctx)) {

			strcpy(info.mimetype, out->mimetype);
			sprintf(info.uri, "http://%s:%hu/" BRIDGE_URL "%d.%s", sq_ip,
					out->port, out->index, mimetype2ext(out->mimetype));

			ret = ctx_callback(ctx, SQ_SET_TRACK, NULL, &info);

			LOG_INFO("[%p]: codec:%c, ch:%d, s:%d, r:%d", ctx, out->codec, out->channels, out->sample_size, out->sample_rate);
		} else sq_free_metadata(&info.metadata);
	}

	return ret;
}











