/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/tests/BcmTest.h"
#include "fboss/agent/platforms/tests/utils/BcmTestPlatform.h"

#include "fboss/agent/ApplyThriftConfig.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/BcmQosUtils.h"

#include "fboss/agent/hw/test/ConfigFactory.h"

extern "C" {
#include <bcm/cosq.h>
#include <bcm/error.h>
#include <bcm/qos.h>
}

namespace {
const std::vector<int> kTrafficClassToPgId{0, 1, 2, 3, 4, 5, 6, 7};
const std::vector<int> kPfcPriorityToPgId{7, 7, 7, 7, 7, 7, 7, 7};
const std::vector<int>
    kTrafficClassToPgIdInHw{0, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0};
} // namespace

namespace facebook::fboss {

class BcmQosMapTest : public BcmTest {
 protected:
  cfg::SwitchConfig initialConfig() const override {
    return utility::oneL3IntfNPortConfig(
        getHwSwitch(), {masterLogicalPortIds()[0], masterLogicalPortIds()[1]});
  }

  cfg::SwitchConfig setupDefaultQueueWithPfcMaps() {
    auto config = initialConfig();
    cfg::QosMap qosMap;
    std::map<int16_t, int16_t> tc2PgId;
    std::map<int16_t, int16_t> pfcPri2PgId;
    for (auto i = 0; i < kTrafficClassToPgId.size(); i++) {
      tc2PgId.emplace(i, kTrafficClassToPgId[i]);
    }
    for (auto i = 0; i < kPfcPriorityToPgId.size(); i++) {
      pfcPri2PgId.emplace(i, kPfcPriorityToPgId[i]);
    }

    // setup pfc maps
    qosMap.trafficClassToPgId_ref() = tc2PgId;
    qosMap.pfcPriorityToPgId_ref() = pfcPri2PgId;

    config.qosPolicies_ref()->resize(1);
    config.qosPolicies_ref()[0].name_ref() = "qp";
    config.qosPolicies_ref()[0].qosMap_ref() = qosMap;

    cfg::TrafficPolicyConfig dataPlaneTrafficPolicy;
    dataPlaneTrafficPolicy.defaultQosPolicy_ref() = "qp";
    config.dataPlaneTrafficPolicy_ref() = dataPlaneTrafficPolicy;
    applyNewConfig(config);
    return config;
  }

  void validateTc2PgId(const std::vector<int>& expectedTc2Pg) {
    std::vector<int> tc2PgId;
    tc2PgId.resize(getBcmDefaultTrafficClassToPgSize());
    int arrayCount = 0;
    bcm_cosq_priority_group_mapping_profile_get(
        getUnit(),
        0,
        bcmCosqInputPriPriorityGroupMcMapping,
        getBcmDefaultTrafficClassToPgSize(),
        tc2PgId.data(),
        &arrayCount);
    EXPECT_EQ(arrayCount, expectedTc2Pg.size());
    // all entries should be same
    EXPECT_TRUE(std::equal(
        expectedTc2Pg.begin(),
        expectedTc2Pg.end(),
        tc2PgId.begin(),
        tc2PgId.end()));
  }

  void validatePfcPri2PgId(const std::vector<int>& expectedPfcPri2Pg) {
    std::vector<int> pfcPri2PgId;
    pfcPri2PgId.resize(getBcmDefaultPfcPriorityToPgSize());
    int arrayCount = 0;
    auto rv = bcm_cosq_priority_group_pfc_priority_mapping_profile_get(
        getUnit(),
        getDefaultProfileId(),
        getBcmDefaultPfcPriorityToPgSize(),
        pfcPri2PgId.data(),
        &arrayCount);
    EXPECT_TRUE(BCM_SUCCESS(rv));
    EXPECT_EQ(arrayCount, expectedPfcPri2Pg.size());
    // all entries should be same
    EXPECT_TRUE(std::equal(
        expectedPfcPri2Pg.begin(),
        expectedPfcPri2Pg.end(),
        pfcPri2PgId.begin(),
        pfcPri2PgId.end()));
  }
};

TEST_F(BcmQosMapTest, BcmNumberOfQoSMaps) {
  auto setup = [this]() { applyNewConfig(initialConfig()); };

  auto verify = [this]() {
    auto mapIdsAndFlags = getBcmQosMapIdsAndFlags(getUnit());
    // In a previous SDK (6.4.10) we have seen extra QoS Maps show
    // up post warm boot. This is fixed 6.5.13 onwards. Assert so we
    // can catch any future breakages.
    int numEntries = mapIdsAndFlags.size();
    EXPECT_EQ(numEntries, 0);
  };

  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(BcmQosMapTest, BcmDscpMapWithRules) {
  auto setup = [this]() {
    auto config = initialConfig();

    config.qosPolicies_ref()->resize(1);
    *config.qosPolicies_ref()[0].name_ref() = "qp";
    config.qosPolicies_ref()[0].rules_ref()->resize(8);
    for (auto i = 0; i < 8; i++) {
      config.qosPolicies_ref()[0].rules_ref()[i].dscp_ref()->resize(8);
      for (auto j = 0; j < 8; j++) {
        config.qosPolicies_ref()[0].rules_ref()[i].dscp_ref()[j] = 8 * i + j;
      }
    }
    cfg::TrafficPolicyConfig dataPlaneTrafficPolicy;
    dataPlaneTrafficPolicy.defaultQosPolicy_ref() = "qp";
    config.dataPlaneTrafficPolicy_ref() = dataPlaneTrafficPolicy;
    applyNewConfig(config);
  };
  auto verify = [this]() {
    auto mapIdsAndFlags = getBcmQosMapIdsAndFlags(getUnit());
    int numEntries = mapIdsAndFlags.size();
    EXPECT_EQ(numEntries, 3); // by default setup ingress & egress mpls qos maps
    for (auto mapIdAndFlag : mapIdsAndFlags) {
      auto mapId = mapIdAndFlag.first;
      auto flag = mapIdAndFlag.second;
      if ((flag & (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3)) ==
          (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3)) {
        int array_count = 0;
        bcm_qos_map_multi_get(getUnit(), flag, mapId, 0, nullptr, &array_count);
        EXPECT_EQ(
            (flag & (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3)),
            (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3));
        EXPECT_EQ(array_count, 64);
      }
    }
  };
  verifyAcrossWarmBoots(setup, verify);
}

// configure trafficClassToPg map, remove the qos
// policy so that trafficClassToPg map is reset  to default
// Query HW to validate the same
TEST_F(BcmQosMapTest, PfcMapsRemovePolicy) {
  if (!isSupported(HwAsic::Feature::PFC)) {
    XLOG(WARNING) << "Platform doesn't support PFC";
    return;
  }

  auto setup = [this]() {
    auto config = setupDefaultQueueWithPfcMaps();
    // reset qosPolicy
    config = initialConfig();
    applyNewConfig(config);
  };

  auto verify = [this]() {
    validateTc2PgId(getBcmDefaultTrafficClassToPgArr());
    validatePfcPri2PgId(getBcmDefaultPfcPriorityToPgArr());
  };
  verifyAcrossWarmBoots(setup, verify);
}

// configure trafficClassToPg map and remove it
// explicitly, so that defaults get programmed
// Query HW to validate the same.
// Since we reset the trafficClassToPg map explicitly
// it takes a diffeent code path from Tc2PgRemovePolicy
TEST_F(BcmQosMapTest, PfcMapsReset) {
  if (!isSupported(HwAsic::Feature::PFC)) {
    XLOG(WARNING) << "Platform doesn't support PFC";
    return;
  }

  auto setup = [this]() {
    cfg::QosMap qosMap;
    auto config = setupDefaultQueueWithPfcMaps();
    // reset TC <-> PG Id
    qosMap.trafficClassToPgId_ref().reset();
    qosMap.pfcPriorityToPgId_ref().reset();
    config.qosPolicies_ref()[0].qosMap_ref() = qosMap;
    applyNewConfig(config);
  };

  auto verify = [this]() {
    validateTc2PgId(getBcmDefaultTrafficClassToPgArr());
    validatePfcPri2PgId(getBcmDefaultPfcPriorityToPgArr());
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(BcmQosMapTest, BcmAllQosMapsWithPfcMaps) {
  if (!isSupported(HwAsic::Feature::PFC)) {
    XLOG(WARNING) << "Platform doesn't support PFC";
    return;
  }

  auto setup = [this]() {
    auto config = initialConfig();

    cfg::QosMap qosMap;
    qosMap.dscpMaps_ref()->resize(8);
    for (auto i = 0; i < 8; i++) {
      qosMap.dscpMaps_ref()[i].internalTrafficClass_ref() = i;
      for (auto j = 0; j < 8; j++) {
        qosMap.dscpMaps_ref()[i].fromDscpToTrafficClass_ref()->push_back(
            8 * i + j);
      }
    }
    qosMap.expMaps_ref()->resize(8);
    for (auto i = 0; i < 8; i++) {
      qosMap.expMaps_ref()[i].internalTrafficClass_ref() = i;
      qosMap.expMaps_ref()[i].fromExpToTrafficClass_ref()->push_back(i);
      qosMap.expMaps_ref()[i].fromTrafficClassToExp_ref() = i;
    }

    std::map<int16_t, int16_t> tc2PgId;
    std::map<int16_t, int16_t> pfcPri2PgId;

    for (auto i = 0; i < kTrafficClassToPgId.size(); i++) {
      // add trafficClassToPgId mappings as well
      tc2PgId.emplace(i, kTrafficClassToPgId[i]);
    }

    for (auto i = 0; i < kPfcPriorityToPgId.size(); ++i) {
      // add pfc priority to PG id mappings
      pfcPri2PgId.emplace(i, kPfcPriorityToPgId[i]);
    }
    qosMap.trafficClassToPgId_ref() = tc2PgId;
    qosMap.pfcPriorityToPgId_ref() = pfcPri2PgId;

    config.qosPolicies_ref()->resize(1);
    config.qosPolicies_ref()[0].name_ref() = "qp";
    config.qosPolicies_ref()[0].qosMap_ref() = qosMap;

    cfg::TrafficPolicyConfig dataPlaneTrafficPolicy;
    dataPlaneTrafficPolicy.defaultQosPolicy_ref() = "qp";
    config.dataPlaneTrafficPolicy_ref() = dataPlaneTrafficPolicy;
    applyNewConfig(config);
  };

  auto verify = [this]() {
    auto mapIdsAndFlags = getBcmQosMapIdsAndFlags(getUnit());
    int numEntries = mapIdsAndFlags.size();
    EXPECT_EQ(
        numEntries, 3); // 3 qos maps (ingress dscp, ingress mpls, egress mpls)
    for (auto mapIdAndFlag : mapIdsAndFlags) {
      auto mapId = mapIdAndFlag.first;
      auto flag = mapIdAndFlag.second;
      int array_count = 0;
      bcm_qos_map_multi_get(getUnit(), flag, mapId, 0, nullptr, &array_count);
      if ((flag & (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3)) ==
          (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3)) {
        EXPECT_EQ(array_count, 64);
      }
      if ((flag & (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_MPLS)) ==
          (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_MPLS)) {
        EXPECT_EQ(array_count, 8);
      }
      if ((flag & (BCM_QOS_MAP_EGRESS | BCM_QOS_MAP_MPLS)) ==
          (BCM_QOS_MAP_EGRESS | BCM_QOS_MAP_MPLS)) {
        // TH4 always return 48 entries not including ghost ones when
        // qos_map_multi_get_mode is 1.
        EXPECT_TRUE(array_count == 64 || array_count == 48);
        std::vector<bcm_qos_map_t> entries;
        entries.resize(array_count);
        bcm_qos_map_multi_get(
            getUnit(),
            flag,
            mapId,
            entries.size(),
            entries.data(),
            &array_count);
        // not returning any invalid or ghost entries now
        EXPECT_EQ(array_count, 48);
      }
    }
    validateTc2PgId(kTrafficClassToPgIdInHw);
    validatePfcPri2PgId(kPfcPriorityToPgId);
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(BcmQosMapTest, BcmAllQosMaps) {
  auto setup = [this]() {
    auto config = initialConfig();

    cfg::QosMap qosMap;
    qosMap.dscpMaps_ref()->resize(8);
    for (auto i = 0; i < 8; i++) {
      *qosMap.dscpMaps_ref()[i].internalTrafficClass_ref() = i;
      for (auto j = 0; j < 8; j++) {
        qosMap.dscpMaps_ref()[i].fromDscpToTrafficClass_ref()->push_back(
            8 * i + j);
      }
    }
    qosMap.expMaps_ref()->resize(8);
    for (auto i = 0; i < 8; i++) {
      *qosMap.expMaps_ref()[i].internalTrafficClass_ref() = i;
      qosMap.expMaps_ref()[i].fromExpToTrafficClass_ref()->push_back(i);
      qosMap.expMaps_ref()[i].fromTrafficClassToExp_ref() = i;
    }

    for (auto i = 0; i < 8; i++) {
      qosMap.trafficClassToQueueId_ref()->emplace(i, i);
    }
    config.qosPolicies_ref()->resize(1);
    *config.qosPolicies_ref()[0].name_ref() = "qp";
    config.qosPolicies_ref()[0].qosMap_ref() = qosMap;

    cfg::TrafficPolicyConfig dataPlaneTrafficPolicy;
    dataPlaneTrafficPolicy.defaultQosPolicy_ref() = "qp";
    config.dataPlaneTrafficPolicy_ref() = dataPlaneTrafficPolicy;
    applyNewConfig(config);
  };

  auto verify = [this]() {
    auto mapIdsAndFlags = getBcmQosMapIdsAndFlags(getUnit());
    int numEntries = mapIdsAndFlags.size();
    EXPECT_EQ(
        numEntries, 3); // 3 qos maps (ingress dscp, ingress mpls, egress mpls)
    for (auto mapIdAndFlag : mapIdsAndFlags) {
      auto mapId = mapIdAndFlag.first;
      auto flag = mapIdAndFlag.second;
      int array_count = 0;
      bcm_qos_map_multi_get(getUnit(), flag, mapId, 0, nullptr, &array_count);
      if ((flag & (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3)) ==
          (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_L3)) {
        EXPECT_EQ(array_count, 64);
      }
      if ((flag & (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_MPLS)) ==
          (BCM_QOS_MAP_INGRESS | BCM_QOS_MAP_MPLS)) {
        EXPECT_EQ(array_count, 8);
      }
      if ((flag & (BCM_QOS_MAP_EGRESS | BCM_QOS_MAP_MPLS)) ==
          (BCM_QOS_MAP_EGRESS | BCM_QOS_MAP_MPLS)) {
        // TH4 always return 48 entries not including ghost ones when
        // qos_map_multi_get_mode is 1.
        EXPECT_TRUE(array_count == 64 || array_count == 48);
        std::vector<bcm_qos_map_t> entries;
        entries.resize(array_count);
        bcm_qos_map_multi_get(
            getUnit(),
            flag,
            mapId,
            entries.size(),
            entries.data(),
            &array_count);
        // not returning any invalid or ghost entries now
        EXPECT_EQ(array_count, 48);
      }
    }
  };
  verifyAcrossWarmBoots(setup, verify);
}

} // namespace facebook::fboss
