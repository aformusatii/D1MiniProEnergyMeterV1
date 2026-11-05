#ifndef _TelegramBot_H_
#define _TelegramBot_H_

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <functional>

#include "Logger.h"

#define TELEGRAM_HOST              "api.telegram.org"
#define TELEGRAM_PORT              443

// Long polling: Telegram holds getUpdates open until a message arrives or this timeout ends
#define TELEGRAM_POLL_TIMEOUT_S     30
#define TELEGRAM_READ_SLACK_MS   15000
#define TELEGRAM_SEND_TIMEOUT_MS 15000
#define TELEGRAM_TLS_TIMEOUT_MS  10000
#define TELEGRAM_RETRY_MIN_MS     5000
#define TELEGRAM_RETRY_MAX_MS   300000

// api.telegram.org supports TLS max fragment length negotiation, so BearSSL can use 1 KB
// buffers instead of 16 KB. Without it (probe fails) the full size buffers are used.
#define TELEGRAM_TLS_BUF          1024
// free heap needed on top of the TLS buffers for the BearSSL context and the rest of the app
#define TELEGRAM_HEAP_RESERVE    12000

#define TELEGRAM_BODY_MAX         1536
#define TELEGRAM_TEXT_MAX           64
#define TELEGRAM_MSG_MAX           640
// e.g. your private chat and a family group
#define TELEGRAM_MAX_CHATS           2

// commands sent while the device was offline (e.g. during a power cut) are ignored when older
#define TELEGRAM_MAX_AGE_S         300
#define TELEGRAM_TIME_VALID_AFTER  1700000000

typedef std::function<void(int64_t chatId, const char *text)> TelegramHandler;

/*
 * Minimal non-blocking Telegram Bot API client (getUpdates long polling + sendMessage) over one
 * kept-alive TLS connection. One bot serves several chats (addChat()): only messages from those
 * chats reach the handler, together with the chat id, so the caller can route them. Others are
 * logged with their chat id and ignored.
 *
 * One message can be queued at a time (send()), to any of the chats. A queued reply interrupts a running long poll, which
 * is safe: an update counts as received only once the next getUpdates confirms its offset.
 * Only the TLS connect (~1-2 s, rare with keep-alive) blocks loop().
 */
class TelegramBot {

	private:
		WiFiClientSecure client;
		Logger *logger;
		TelegramHandler handler;

		const char *token;
		int64_t chats[TELEGRAM_MAX_CHATS];
		uint8_t chatCount;
		int64_t unknownChat;

		uint8_t state;
		unsigned long stateTime;
		unsigned long retryDelay;
		unsigned long readTimeout;
		int error;

		int64_t nextOffset;
		bool requestIsPoll;

		// response parsing
		char line[128];
		uint8_t lineLen;
		int httpStatus;
		long contentLength;
		long bodyRead;
		size_t bodyLen;

		char outbox[TELEGRAM_MSG_MAX];
		bool outboxFull;
		int64_t outboxChat;
		const char *outboxMarkup;

		bool connect(void);
		void sendRequest(unsigned long current_time);
		void readResponse(unsigned long current_time);
		void headerLine(void);
		void finish(unsigned long current_time);
		void processUpdates(bool truncated);
		void handleMessage(JsonObject message);
		void fail(unsigned long current_time, int code, const char *msg);
		void backoff(void);

	public:
		TelegramBot(void);

		// token nullptr or empty disables the bot
		void begin(const char *token, Logger *logger, TelegramHandler handler);

		// Accepts messages from this chat (user id, or negative group id). 0 is ignored.
		bool addChat(int64_t chatId);

		// Queues one message to chatId. replyMarkup is reply_markup JSON (e.g. a keyboard), it must
		// stay valid until the message is sent. Returns false if one is already waiting.
		bool send(int64_t chatId, const char *text, const char *replyMarkup = nullptr);

		// Call from loop() while WiFi is connected
		void loop(unsigned long current_time);

		bool enabled(void) { return token != nullptr; }
		// true while the last request succeeded
		bool ok(void) { return enabled() && error == 0 && client.connected(); }
		// 0 = ok, HTTP status from Telegram (401 bad token, 409 another poller, ...), -1 connection, -2 low heap
		int lastError(void) { return error; }
		// last chat the bot got a message from that is not allowed, 0 = none (for setting up chat ids)
		int64_t lastUnknownChat(void) { return unknownChat; }
};

#endif /* _TelegramBot_H_ */
