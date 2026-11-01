#ifndef _WiFiConfig_H_
#define _WiFiConfig_H_

// Copy this file to include/WiFiConfig.h and put the real credentials there.
// include/WiFiConfig.h is ignored by git.

static const char LOCAL2_SSID[] = "your-ssid";
static const char LOCAL2_KEY[]  = "your-password";

// Telegram bot for Wake-on-LAN (README, "Telegram bot"). Leave commented out to disable the bot.
// The token comes from @BotFather, the chat id is your own Telegram user id. Only messages from
// that chat are accepted.
// #define TELEGRAM_BOT_TOKEN  "123456789:AAExampleExampleExampleExampleExam"
// #define TELEGRAM_CHAT_ID    123456789LL

#endif /* _WiFiConfig_H_ */
