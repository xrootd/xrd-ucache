#include "ClientSettings.h"

namespace ucache {

const ClientSetting kClientSettings[kClientSettingCount] = {
    {"ConnectionWindow", "XRD_CONNECTIONWINDOW", 31},
    {"ConnectionRetry", "XRD_CONNECTIONRETRY", 2},
    {"StreamErrorWindow", "XRD_STREAMERRORWINDOW", 180},
};

ClientSettingsPlan planClientSettings(bool switchOn, const bool envSet[kClientSettingCount],
                                      const bool currentEqualsDefault[kClientSettingCount]) {
  ClientSettingsPlan p{};
  for (int i = 0; i < kClientSettingCount; ++i)
    p.step[i] = !switchOn                 ? ClientSettingStep::kLeave
                : envSet[i]               ? ClientSettingStep::kKeepEnv
                : !currentEqualsDefault[i] ? ClientSettingStep::kKeepClient
                                          : ClientSettingStep::kApply;
  return p;
}

const char* clientSettingSource(ClientSettingStep step, bool applied, bool envSet,
                                bool currentEqualsDefault) {
  switch (step) {
  case ClientSettingStep::kApply:
    return applied ? "ucache" : "kept";
  case ClientSettingStep::kKeepEnv:
    return "kept";
  case ClientSettingStep::kKeepClient:
    return "client";
  case ClientSettingStep::kLeave:
    break;
  }
  return envSet ? "env" : currentEqualsDefault ? "default" : "client";
}

} // namespace ucache
