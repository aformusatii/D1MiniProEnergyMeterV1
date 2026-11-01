#include "TelegramBot.h"

#include <ArduinoJson.h>

#define STATE_DISABLED  0
#define STATE_IDLE      1  // next request after retryDelay
#define STATE_HEADERS   2
#define STATE_BODY      3

#define ERROR_CONNECTION  -1
#define ERROR_LOW_HEAP    -2

// overhead BearSSL adds to each buffer (WiFiClientSecureCtx::setBufferSizes)
#define TLS_BUF_OVERHEAD  410

static char body[TELEGRAM_BODY_MAX + 1];

TelegramBot::TelegramBot() {
	logger = nullptr;
	token = nullptr;
	chatId = 0;
	replyMarkup = nullptr;

	state = STATE_DISABLED;
	stateTime = 0;
	retryDelay = 0;
	readTimeout = 0;
	error = 0;

	nextOffset = 0;
	requestIsPoll = true;

	lineLen = 0;
	httpStatus = 0;
	contentLength = -1;
	bodyRead = 0;
	bodyLen = 0;

	outbox[0] = 0;
	outboxFull = false;
	outboxKeyboard = false;
}

void TelegramBot::begin(const char *_token, int64_t _chatId, Logger *_logger, TelegramHandler _handler) {
	logger = _logger;
	handler = _handler;

	if (_token == nullptr || _token[0] == 0) {
		logger->info("Telegram bot disabled (no token)");
		return;
	}

	token = _token;
	chatId = _chatId;
	state = STATE_IDLE;

	// no certificate check: a CA change on Telegram's side must not lock out the only remote access
	client.setInsecure();
	client.setTimeout(TELEGRAM_TLS_TIMEOUT_MS);
}

bool TelegramBot::send(const char *text, bool keyboard) {
	if (!enabled() || outboxFull) {
		return false;
	}

	strlcpy(outbox, text, sizeof(outbox));
	outboxKeyboard = keyboard;
	outboxFull = true;
	return true;
}

void TelegramBot::loop(unsigned long current_time) {
	switch (state) {

		case STATE_DISABLED:
			break;

		case STATE_IDLE:
			if ((current_time - stateTime) < retryDelay) {
				break;
			}
			if (!client.connected() && !connect()) {
				fail(current_time, error, "Telegram connect failed");
				break;
			}
			sendRequest(current_time);
			break;

		case STATE_HEADERS:
		case STATE_BODY:
			if (outboxFull && requestIsPoll && httpStatus == 0 && lineLen == 0) {
				// nothing of the poll answer read yet: drop it to send the reply now,
				// an update in flight is delivered again by the next getUpdates
				client.stop();
				state = STATE_IDLE;
				stateTime = current_time;
				break;
			}
			readResponse(current_time);
			break;
	}
}

bool TelegramBot::connect() {
	bool mfln = WiFiClientSecure::probeMaxFragmentLength(TELEGRAM_HOST, TELEGRAM_PORT, TELEGRAM_TLS_BUF);
	int in = mfln ? TELEGRAM_TLS_BUF : 16384;
	int out = mfln ? TELEGRAM_TLS_BUF : 512;

	uint32_t heap = ESP.getFreeHeap();
	if (heap < (uint32_t) (in + out + 2 * TLS_BUF_OVERHEAD + TELEGRAM_HEAP_RESERVE)) {
		JsonDocument doc;
		doc["heap"] = heap;
		doc["mfln"] = mfln;
		logger->error(doc, "Telegram: not enough heap for TLS");
		error = ERROR_LOW_HEAP;
		return false;
	}

	client.setBufferSizes(in, out);

	unsigned long start = millis();
	bool connected = client.connect(TELEGRAM_HOST, TELEGRAM_PORT);

	JsonDocument doc;
	doc["mfln"] = mfln;
	doc["ms"] = millis() - start;
	doc["heap"] = ESP.getFreeHeap();
	if (!connected) {
		doc["ssl_error"] = client.getLastSSLError();
		error = ERROR_CONNECTION;
		logger->error(doc, "Telegram TLS connect failed");
		return false;
	}

	logger->info(doc, "Telegram connected");
	return true;
}

void TelegramBot::sendRequest(unsigned long current_time) {
	requestIsPoll = !outboxFull;

	JsonDocument doc;
	const char *method;
	if (requestIsPoll) {
		method = "getUpdates";
		doc["timeout"] = TELEGRAM_POLL_TIMEOUT_S;
		doc["limit"] = 1;
		doc["allowed_updates"][0] = "message";
		if (nextOffset != 0) {
			doc["offset"] = nextOffset;
		}
		readTimeout = TELEGRAM_POLL_TIMEOUT_S * 1000UL + TELEGRAM_READ_SLACK_MS;
	} else {
		method = "sendMessage";
		doc["chat_id"] = chatId;
		doc["text"] = outbox;
		if (outboxKeyboard && replyMarkup != nullptr) {
			doc["reply_markup"] = serialized(replyMarkup);
		}
		readTimeout = TELEGRAM_SEND_TIMEOUT_MS;
	}

	char payload[TELEGRAM_MSG_MAX + 300];
	size_t payloadLen = serializeJson(doc, payload, sizeof(payload));

	char header[256];
	int headerLen = snprintf(header, sizeof(header),
			"POST /bot%s/%s HTTP/1.1\r\nHost: " TELEGRAM_HOST "\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n",
			token, method, (unsigned) payloadLen);

	lineLen = 0;
	httpStatus = 0;
	contentLength = -1;
	bodyRead = 0;
	bodyLen = 0;

	if (client.write((const uint8_t *) header, headerLen) != (size_t) headerLen
			|| client.write((const uint8_t *) payload, payloadLen) != payloadLen) {
		fail(current_time, ERROR_CONNECTION, "Telegram request write failed");
		return;
	}

	state = STATE_HEADERS;
	stateTime = current_time;
}

void TelegramBot::readResponse(unsigned long current_time) {
	while ((state == STATE_HEADERS || state == STATE_BODY) && client.available() > 0) {
		if (state == STATE_HEADERS) {
			int c = client.read();
			if (c < 0) {
				break;
			}
			if (c == '\n') {
				line[lineLen] = 0;
				headerLine();
				lineLen = 0;
			} else if (c != '\r' && lineLen < sizeof(line) - 1) {
				line[lineLen++] = (char) c;
			}
			if (state == STATE_BODY && contentLength == 0) {
				finish(current_time);
			}
			continue;
		}

		// body: keep what fits, skip the rest (only update_id is needed from an oversized update)
		uint8_t chunk[256];
		size_t want = min((size_t) (contentLength - bodyRead), sizeof(chunk));
		int n = client.read(chunk, want);
		if (n <= 0) {
			break;
		}
		size_t keep = min((size_t) n, (size_t) TELEGRAM_BODY_MAX - bodyLen);
		memcpy(body + bodyLen, chunk, keep);
		bodyLen += keep;
		bodyRead += n;

		if (bodyRead >= contentLength) {
			finish(current_time);
		}
	}

	if (state != STATE_HEADERS && state != STATE_BODY) {
		return;
	}

	if (state == STATE_HEADERS && httpStatus == -1) {
		fail(current_time, ERROR_CONNECTION, "Telegram: bad HTTP response");
	} else if (!client.connected()) {
		fail(current_time, ERROR_CONNECTION, "Telegram connection closed");
	} else if ((current_time - stateTime) > readTimeout) {
		fail(current_time, ERROR_CONNECTION, "Telegram read timeout");
	}
}

void TelegramBot::headerLine() {
	if (httpStatus == 0) {
		// status line: HTTP/1.1 200 OK
		httpStatus = (strncmp(line, "HTTP/1.", 7) == 0) ? atoi(line + 9) : -1;
		if (httpStatus <= 0) {
			httpStatus = -1;
		}
		return;
	}

	if (lineLen == 0) {
		// end of headers, Telegram always sends Content-Length (no chunked encoding)
		if (contentLength < 0) {
			httpStatus = -1;
			return;
		}
		state = STATE_BODY;
		return;
	}

	if (strncasecmp(line, "Content-Length:", 15) == 0) {
		contentLength = atol(line + 15);
	}
}

void TelegramBot::finish(unsigned long current_time) {
	body[bodyLen] = 0;
	state = STATE_IDLE;
	stateTime = current_time;

	if (httpStatus == 200) {
		error = 0;
		retryDelay = 0;
		if (requestIsPoll) {
			processUpdates(bodyRead > (long) bodyLen);
		} else {
			outboxFull = false;
		}
		return;
	}

	// error answers look like {"ok":false,"error_code":401,"description":"Unauthorized"}
	JsonDocument doc;
	doc["http"] = httpStatus;
	doc["method"] = requestIsPoll ? "getUpdates" : "sendMessage";
	JsonDocument answer;
	if (!deserializeJson(answer, body) && answer["description"].is<const char *>()) {
		doc["description"] = answer["description"];
	}
	logger->error(doc, "Telegram request failed");

	error = httpStatus;
	if (httpStatus == 401 || httpStatus == 404) {
		// wrong token: nothing to gain by retrying often
		retryDelay = TELEGRAM_RETRY_MAX_MS;
	} else {
		backoff();
	}

	if (!requestIsPoll && httpStatus >= 400 && httpStatus < 500 && httpStatus != 429) {
		// e.g. wrong chat id, the message would never go through
		outboxFull = false;
	}
}

void TelegramBot::processUpdates(bool truncated) {
	JsonDocument filter;
	filter["result"][0]["update_id"] = true;
	JsonObject messageFilter = filter["result"][0]["message"].to<JsonObject>();
	messageFilter["date"] = true;
	messageFilter["text"] = true;
	messageFilter["chat"]["id"] = true;

	JsonDocument doc;
	if (truncated || deserializeJson(doc, body, bodyLen, DeserializationOption::Filter(filter))) {
		// skip the update by its id, otherwise it would be delivered again forever
		const char *id = strstr(body, "\"update_id\":");
		JsonDocument logDoc;
		logDoc["size"] = bodyRead;
		if (id != nullptr) {
			nextOffset = strtoll(id + 12, nullptr, 10) + 1;
			logger->error(logDoc, "Telegram update too large or invalid, skipped");
		} else {
			logger->error(logDoc, "Telegram getUpdates answer not understood");
		}
		return;
	}

	for (JsonObject update : doc["result"].as<JsonArray>()) {
		nextOffset = update["update_id"].as<int64_t>() + 1;
		handleMessage(update["message"]);
	}
}

void TelegramBot::handleMessage(JsonObject message) {
	if (message.isNull()) {
		return;
	}

	int64_t from = message["chat"]["id"];
	const char *text = message["text"] | "";

	JsonDocument doc;
	doc["chat_id"] = from;
	doc["text"] = text;

	if (from != chatId) {
		// the chat id shows up in the logs this way, also handy when setting up TELEGRAM_CHAT_ID
		logger->error(doc, "Telegram message from unknown chat ignored");
		return;
	}

	time_t now = time(nullptr);
	uint32_t date = message["date"];
	if (now > TELEGRAM_TIME_VALID_AFTER && (int64_t) date + TELEGRAM_MAX_AGE_S < (int64_t) now) {
		doc["age_s"] = (int64_t) now - date;
		logger->info(doc, "Telegram: old command ignored");
		return;
	}

	logger->info(doc, "Telegram command");

	char command[TELEGRAM_TEXT_MAX];
	strlcpy(command, text, sizeof(command));
	handler(command);
}

void TelegramBot::fail(unsigned long current_time, int code, const char *msg) {
	client.stop();
	error = code;
	backoff();
	state = STATE_IDLE;
	stateTime = current_time;

	JsonDocument doc;
	doc["retryMs"] = retryDelay;
	logger->error(doc, msg);
}

void TelegramBot::backoff() {
	retryDelay = (retryDelay == 0) ? TELEGRAM_RETRY_MIN_MS : min(retryDelay * 2, (unsigned long) TELEGRAM_RETRY_MAX_MS);
}
