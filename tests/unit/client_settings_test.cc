// The rule for the XRootD client's connection settings under
// adaptive_federation (ClientSettings.h): set only with the switch on, and
// never over a value an XRD_* variable or a client.conf chose. Pure; that the
// values reach the client is checked where the plugin runs.
#include "ClientSettings.h"

#include <gtest/gtest.h>
#include <string>

using namespace ucache;

// The three settings, their variables and the values the switch sets.
TEST(ClientSettings, Table) {
  EXPECT_STREQ(kClientSettings[0].name, "ConnectionWindow");
  EXPECT_STREQ(kClientSettings[0].envName, "XRD_CONNECTIONWINDOW");
  EXPECT_EQ(kClientSettings[0].federationValue, 31);
  EXPECT_STREQ(kClientSettings[1].name, "ConnectionRetry");
  EXPECT_STREQ(kClientSettings[1].envName, "XRD_CONNECTIONRETRY");
  EXPECT_EQ(kClientSettings[1].federationValue, 2);
  EXPECT_STREQ(kClientSettings[2].name, "StreamErrorWindow");
  EXPECT_STREQ(kClientSettings[2].envName, "XRD_STREAMERRORWINDOW");
  EXPECT_EQ(kClientSettings[2].federationValue, 180);
}

// Every combination of (switch, variable set, value at the client's default)
// for each setting, on its own and beside the others: a setting's step and
// source depend on its own inputs only.
TEST(ClientSettings, ApplyOrKeepForEveryCombination) {
  struct Row {
    bool on, env, dflt;
    ClientSettingStep step;
    const char* srcApplied; // the source when the client accepts the write
    const char* srcRefused; // ... and when it refuses it
  };
  const Row rows[] = {
      {false, false, true, ClientSettingStep::kLeave, "default", "default"},
      {false, false, false, ClientSettingStep::kLeave, "client", "client"},
      {false, true, true, ClientSettingStep::kLeave, "env", "env"},
      {false, true, false, ClientSettingStep::kLeave, "env", "env"},
      {true, false, true, ClientSettingStep::kApply, "ucache", "kept"},
      {true, false, false, ClientSettingStep::kKeepClient, "client", "client"},
      {true, true, true, ClientSettingStep::kKeepEnv, "kept", "kept"},
      {true, true, false, ClientSettingStep::kKeepEnv, "kept", "kept"},
  };
  for (const Row& r : rows)
    for (int i = 0; i < kClientSettingCount; ++i) {
      const std::string at = std::string("on=") + (r.on ? "1" : "0") + " env=" +
                             (r.env ? "1" : "0") + " default=" + (r.dflt ? "1" : "0") +
                             " setting " + std::to_string(i);
      // The others carry the opposite inputs, so a mix-up between them shows.
      bool env[kClientSettingCount], dflt[kClientSettingCount];
      for (int j = 0; j < kClientSettingCount; ++j) {
        env[j] = j == i ? r.env : !r.env;
        dflt[j] = j == i ? r.dflt : !r.dflt;
      }
      const ClientSettingsPlan p = planClientSettings(r.on, env, dflt);
      EXPECT_EQ(p.step[i], r.step) << at;
      EXPECT_STREQ(clientSettingSource(p.step[i], true, r.env, r.dflt), r.srcApplied) << at;
      EXPECT_STREQ(clientSettingSource(p.step[i], false, r.env, r.dflt), r.srcRefused) << at;
    }
}
