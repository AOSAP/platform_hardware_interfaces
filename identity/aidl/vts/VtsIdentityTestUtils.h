/*
 * Copyright 2019, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VTS_IDENTITY_TEST_UTILS_H
#define VTS_IDENTITY_TEST_UTILS_H

#include <android/hardware/identity/IIdentityCredentialStore.h>
#include <android/hardware/identity/support/IdentityCredentialSupport.h>
#include <cppbor.h>
#include <cppbor_parse.h>

namespace android::hardware::identity::test_utils {

using ::std::map;
using ::std::optional;
using ::std::string;
using ::std::vector;

using ::android::sp;
using ::android::binder::Status;

struct AttestationData {
    AttestationData(sp<IWritableIdentityCredential>& writableCredential, string challenge,
                    vector<uint8_t> applicationId)
        : attestationApplicationId(applicationId) {
        // ASSERT_NE(writableCredential, nullptr);

        if (!challenge.empty()) {
            attestationChallenge.assign(challenge.begin(), challenge.end());
        }

        result = writableCredential->getAttestationCertificate(
                attestationApplicationId, attestationChallenge, &attestationCertificate);
    }

    AttestationData() {}

    vector<uint8_t> attestationChallenge;
    vector<uint8_t> attestationApplicationId;
    vector<Certificate> attestationCertificate;
    Status result;
};

struct TestEntryData {
    TestEntryData(string nameSpace, string name, vector<int32_t> profileIds)
        : nameSpace(nameSpace), name(name), profileIds(profileIds) {}

    TestEntryData(string nameSpace, string name, const string& value, vector<int32_t> profileIds)
        : TestEntryData(nameSpace, name, profileIds) {
        valueCbor = cppbor::Tstr(((const char*)value.data())).encode();
    }
    TestEntryData(string nameSpace, string name, const vector<uint8_t>& value,
                  vector<int32_t> profileIds)
        : TestEntryData(nameSpace, name, profileIds) {
        valueCbor = cppbor::Bstr(value).encode();
    }
    TestEntryData(string nameSpace, string name, bool value, vector<int32_t> profileIds)
        : TestEntryData(nameSpace, name, profileIds) {
        valueCbor = cppbor::Bool(value).encode();
    }
    TestEntryData(string nameSpace, string name, int64_t value, vector<int32_t> profileIds)
        : TestEntryData(nameSpace, name, profileIds) {
        if (value >= 0) {
            valueCbor = cppbor::Uint(value).encode();
        } else {
            valueCbor = cppbor::Nint(-value).encode();
        }
    }

    string nameSpace;
    string name;
    vector<uint8_t> valueCbor;
    vector<int32_t> profileIds;
};

struct TestProfile {
    uint16_t id;
    vector<uint8_t> readerCertificate;
    bool userAuthenticationRequired;
    uint64_t timeoutMillis;
};

bool SetupWritableCredential(sp<IWritableIdentityCredential>& writableCredential,
                             sp<IIdentityCredentialStore>& credentialStore);

optional<vector<uint8_t>> GenerateReaderCertificate(string serialDecimal);

optional<vector<uint8_t>> GenerateReaderCertificate(string serialDecimal,
                                                    vector<uint8_t>& readerPrivateKey);

optional<vector<SecureAccessControlProfile>> AddAccessControlProfiles(
        sp<IWritableIdentityCredential>& writableCredential,
        const vector<TestProfile>& testProfiles);

bool AddEntry(sp<IWritableIdentityCredential>& writableCredential, const TestEntryData& entry,
              int dataChunkSize, map<const TestEntryData*, vector<vector<uint8_t>>>& encryptedBlobs,
              bool expectSuccess);

bool ValidateAttestationCertificate(vector<Certificate>& inputCertificates);

void SetImageData(vector<uint8_t>& image);

}  // namespace android::hardware::identity::test_utils

#endif  // VTS_IDENTITY_TEST_UTILS_H