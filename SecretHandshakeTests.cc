//
// SecretHandshakeTests.cc
//
// Copyright © 2021 Jens Alfke. All rights reserved.
//
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "SecretHandshake.hh"
#include "SecretStream.hh"
#include <sodium.h>
#include <iostream>

#include "catch.hpp"        // https://github.com/catchorg/Catch2

using namespace std;
using namespace snej::shs;


template <size_t SIZE>
static void randomize(std::array<uint8_t,SIZE> &array) {
    randombytes_buf(array.data(), SIZE);
}

template <size_t SIZE>
static string hexString(std::array<uint8_t,SIZE> const& array) {
    string hex;
    hex.resize(2 * SIZE);
    sodium_bin2hex(hex.data(), hex.size() + 1, array.data(), array.size());
    return hex;
}


TEST_CASE("SecretKey", "[Net]") {
    SecretKey sk = SecretKey::generate();
    PublicKey pk = sk.publicKey();
    SecretKeySeed seed = sk.seed();

    SecretKey sk2 = SecretKey(seed);
    PublicKey pk2 = sk2.publicKey();
    CHECK(sk2 == sk);
    CHECK(pk2 == pk);
}


TEST_CASE("AppID", "[Net]") {
    AppID id = Context::appIDFromString("");
    CHECK(hexString(id) == "0000000000000000000000000000000000000000000000000000000000000000");
    id = Context::appIDFromString("ABCDEF");
    CHECK(hexString(id) == "4142434445460000000000000000000000000000000000000000000000000000");
    id = Context::appIDFromString("A string that is too long to fit in an AppID");
    CHECK(hexString(id) == "4120737472696e67207468617420697320746f6f206c6f6e6720746f20666974");
}


struct HandshakeTest {
    SecretKey serverKey, clientKey;
    ServerHandshake server;
    ClientHandshake client;

    HandshakeTest()
    :serverKey(SecretKey::generate())
    ,clientKey(SecretKey::generate())
    ,server({"App", serverKey})
    ,client({"App", clientKey}, serverKey.publicKey())
    { }

    bool sendFromTo(Handshake &src, Handshake &dst, size_t expectedCount) {
        // One step of the handshake:
        CHECK(src.bytesToRead().second == 0);
        CHECK(dst.bytesToSend().second == 0);
        auto toSend = src.bytesToSend();
        CHECK(toSend.second == expectedCount);
        auto toRead = dst.bytesToRead();
        CHECK(toRead.second == toSend.second);
        memcpy(toRead.first, toSend.first, toSend.second);
        dst.readCompleted();
        src.sendCompleted();
        return !src.failed() && !dst.failed();
    }
};


TEST_CASE_METHOD(HandshakeTest, "Handshake", "[SecretHandshake]") {
    // Run the handshake:
    REQUIRE(sendFromTo(client, server,  64));
    REQUIRE(sendFromTo(server, client,  64));
    REQUIRE(sendFromTo(client, server, 112));
    REQUIRE(sendFromTo(server, client,  80));

    REQUIRE(server.finished());
    REQUIRE(client.finished());

    // Check that they ended up with matching session keys, and each other's public keys:
    Session clientSession = client.session(), serverSession = server.session();
    CHECK(clientSession.encryptionKey   == serverSession.decryptionKey);
    CHECK(clientSession.encryptionNonce == serverSession.decryptionNonce);
    CHECK(clientSession.decryptionKey   == serverSession.encryptionKey);
    CHECK(clientSession.decryptionNonce == serverSession.encryptionNonce);

    CHECK(serverSession.peerPublicKey   == clientKey.publicKey());
    CHECK(clientSession.peerPublicKey   == serverKey.publicKey());
}


TEST_CASE_METHOD(HandshakeTest, "Handshake with wrong server key", "[SecretHandshake]") {
    // Create a client that has the wrong server public key:
    PublicKey badServerKey = client.serverPublicKey();
    badServerKey[17]++;
    ClientHandshake badClient({"App", clientKey}, badServerKey);

    // Run the handshake:
    CHECK(sendFromTo(badClient, server,  64));
    CHECK(sendFromTo(server, badClient,  64));
    CHECK(!sendFromTo(badClient, server, 112));
    CHECK(server.failed());
}


struct SessionTest {
    Session session1, session2;

    SessionTest() {
        randomize(session1.encryptionKey);
        randomize(session1.encryptionNonce);
        randomize(session1.decryptionKey);
        randomize(session1.decryptionNonce);

        session2.encryptionKey   = session1.decryptionKey;
        session2.encryptionNonce = session1.decryptionNonce;
        session2.decryptionKey   = session1.encryptionKey;
        session2.decryptionNonce = session1.encryptionNonce;
    }
};


using getSizeResult = std::pair<CryptoBox::status, size_t>;


TEST_CASE_METHOD(SessionTest, "Encrypted Messages", "[SecretHandshake]") {
    CryptoBox box1(session1), box2(session2);

    // Encrypt a message:
    constexpr const char *kCleartext = "Beware the ides of March. We attack at dawn.";
    input_data inClear = {kCleartext, strlen(kCleartext)};

    // Encrypt:
    Nonce originalNonce = session1.encryptionNonce;
    uint8_t cipherBuf[256] = {};
    output_buffer outCipher = {cipherBuf, 0};
    CHECK(box1.encrypt(inClear, outCipher) == CryptoBox::OutTooSmall);
    outCipher.size = inClear.size;
    CHECK(box1.encrypt(inClear, outCipher) == CryptoBox::OutTooSmall);
    outCipher.size = CryptoBox::encryptedSize(inClear.size);
    CHECK(box1.encrypt(inClear, outCipher) == CryptoBox::Success);
    CHECK(outCipher.data == cipherBuf);
    CHECK(outCipher.size == CryptoBox::encryptedSize(inClear.size));
    CHECK(session1.encryptionNonce != originalNonce);

    // Decrypt:
    uint8_t clearBuf[256] = {};
    CHECK(box2.getDecryptedSize({cipherBuf, 0}) == getSizeResult{CryptoBox::IncompleteInput, 0});
    CHECK(box2.getDecryptedSize({cipherBuf, 1}) == getSizeResult{CryptoBox::IncompleteInput, 0});
#if BOXSTREAM_COMPATIBLE
#else
    CHECK(box2.getDecryptedSize({cipherBuf, 2}) == getSizeResult{CryptoBox::Success, inClear.size});
#endif
    CHECK(box2.getDecryptedSize({cipherBuf, sizeof(cipherBuf)}) == getSizeResult{CryptoBox::Success, inClear.size});

    input_data inCipher = {cipherBuf, 0};
    output_buffer outClear = {clearBuf, sizeof(clearBuf)};
    CHECK(box2.decrypt(inCipher, outClear) == CryptoBox::IncompleteInput);
    inCipher.size = 2;
    CHECK(box2.decrypt(inCipher, outClear) == CryptoBox::IncompleteInput);
    inCipher.size = outCipher.size - 1;
    CHECK(box2.decrypt(inCipher, outClear) == CryptoBox::IncompleteInput);
    inCipher.size = outCipher.size;
    CHECK(box2.decrypt(inCipher, outClear) == CryptoBox::Success);
    CHECK(inCipher.size == 0);
    CHECK(inCipher.data == &cipherBuf[outCipher.size]);
    CHECK(outClear.data == clearBuf);
    CHECK(outClear.size == inClear.size);
    CHECK(memcmp(kCleartext, outClear.data, outClear.size) == 0);

    // Both nonces should still match:
    CHECK(session1.encryptionNonce == session2.decryptionNonce);

    // Encrypt another message:
    constexpr const char *kMoreCleartext = "Alea jacta est";
    inClear = {kMoreCleartext, strlen(kMoreCleartext)};
    outCipher = {cipherBuf, sizeof(cipherBuf)};
    CHECK(box1.encrypt(inClear, outCipher) == CryptoBox::Success);
    CHECK(outCipher.data == cipherBuf);
    CHECK(outCipher.size == CryptoBox::encryptedSize(inClear.size));

    // Decrypt it:
    inCipher = {cipherBuf, sizeof(cipherBuf)};
    outClear = {clearBuf, sizeof(clearBuf)};
    CHECK(box2.decrypt(inCipher, outClear) == CryptoBox::Success);
    CHECK(inCipher.size == sizeof(cipherBuf) - outCipher.size);
    CHECK(inCipher.data == &cipherBuf[outCipher.size]);
    CHECK(outClear.data == clearBuf);
    CHECK(outClear.size == inClear.size);
    CHECK(memcmp(kMoreCleartext, outClear.data, outClear.size) == 0);
}


TEST_CASE_METHOD(SessionTest, "Encrypted Messages Overlapping Buffers", "[SecretHandshake]") {
    CryptoBox box1(session1), box2(session2);

    // Check that it's OK to use the same buffer for the input and the output:
    constexpr const char *kCleartext = "Beware the ides of March. We attack at dawn.";
    char buffer[256];
    strcpy(buffer, kCleartext);
    input_data inClear = {buffer, strlen(kCleartext)};
    output_buffer outCipher = {buffer, sizeof(buffer)};
    CHECK(box1.encrypt(inClear, outCipher) == CryptoBox::Success);

#if !BOXSTREAM_COMPATIBLE
    CHECK(box2.getDecryptedSize({buffer, 2}) == getSizeResult{CryptoBox::Success, inClear.size});
#endif

    input_data inCipher = {buffer, sizeof(buffer)};
    output_buffer outClear = {buffer, sizeof(buffer)};
    CHECK(box2.decrypt(inCipher, outClear) == CryptoBox::Success);
    CHECK(inCipher.size == sizeof(buffer) - outCipher.size);
    CHECK(inCipher.data == &buffer[outCipher.size]);
    CHECK(outClear.data == buffer);
    CHECK(outClear.size == inClear.size);
    CHECK(memcmp(kCleartext, outClear.data, outClear.size) == 0);
}


TEST_CASE_METHOD(SessionTest, "Decryption Stream", "[SecretHandshake]") {
#if BOXSTREAM_COMPATIBLE
    static constexpr size_t kEncOverhead = 34;
#else
    static constexpr size_t kEncOverhead = 18;
#endif

    EncryptionStream enc(session1);
    DecryptionStream dec(session2);
    char cipherBuf[256], clearBuf[256];

    CHECK(dec.pull(clearBuf, sizeof(clearBuf)) == 0);

    auto transfer = [&](size_t nBytes) {
        nBytes = enc.pull(cipherBuf, nBytes);
        CHECK(dec.push(cipherBuf, nBytes));
    };

    // Encrypt a message:
    enc.pushPartial("Hel", 3);
    CHECK(enc.bytesAvailable() == 0);
    enc.pushPartial("lo", 2);
    CHECK(enc.bytesAvailable() == 0);
    enc.flush();
    CHECK(enc.bytesAvailable() == 5 + kEncOverhead);

    // Transfer it in two parts:
    transfer(10);
    CHECK(enc.bytesAvailable() == 5 + kEncOverhead - 10);
    CHECK(dec.bytesAvailable() == 0);
    transfer(100);
    CHECK(enc.bytesAvailable() == 0);
    CHECK(dec.bytesAvailable() == 5);

    // Read it:
    size_t bytesRead = dec.pull(clearBuf, sizeof(clearBuf));
    CHECK(bytesRead == 5);
    CHECK(memcmp(clearBuf, "Hello", 5) == 0);

    // Now add two encrypted mesages, but only transfer the first:
    enc.push(" there", 6);
    enc.pushPartial(", world", 7);
    transfer(100);
    enc.flush();
    CHECK(enc.bytesAvailable() == 7 + kEncOverhead);

    // Now read part of the first:
    CHECK(dec.bytesAvailable() == 6);
    size_t n = dec.pull(&clearBuf[bytesRead], 3);
    CHECK(n == 3);
    bytesRead += n;
    CHECK(memcmp(clearBuf, "Hello th", bytesRead) == 0);

    // Transfer the second:
    transfer(100);
    CHECK(enc.bytesAvailable() == 0);
    CHECK(dec.bytesAvailable() == 10);

    // Read the rest:
    n = dec.pull(&clearBuf[bytesRead], 100);
    CHECK(n == 10);
    bytesRead += n;
    CHECK(memcmp(clearBuf, "Hello there, world", bytesRead) == 0);
    CHECK(dec.pull(&clearBuf[bytesRead], 100) == 0);
    CHECK(dec.bytesAvailable() == 0);
}
