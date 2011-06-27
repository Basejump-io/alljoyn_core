/**
 * @file
 * The KeyStore class manages the storing and loading of key blobs from
 * external storage. The default implementation stores key blobs in a file.
 */

/******************************************************************************
 * Copyright 2010-2011, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/

#include <map>

#include <qcc/platform.h>
#include <qcc/Debug.h>
#include <qcc/String.h>
#include <qcc/Crypto.h>
#include <qcc/Environ.h>
#include <qcc/FileStream.h>
#include <qcc/KeyBlob.h>
#include <qcc/Util.h>
#include <qcc/StringSource.h>
#include <qcc/StringSink.h>
#include <qcc/Thread.h>

#include <alljoyn/KeyStoreListener.h>

#include "KeyStore.h"

#include <Status.h>

#define QCC_MODULE "ALLJOYN_AUTH"

using namespace std;
using namespace qcc;

namespace ajn {


static const uint16_t KeyStoreVersion = 0x0102;
static const uint16_t BackVersion = 0x0101;


QStatus KeyStoreListener::PutKeys(KeyStore& keyStore, const qcc::String& source, const qcc::String& password)
{
    StringSource stringSource(source);
    return keyStore.Pull(stringSource, password);
}

QStatus KeyStoreListener::GetKeys(KeyStore& keyStore, qcc::String& sink)
{
    StringSink stringSink;
    QStatus status = keyStore.Push(stringSink);
    if (status == ER_OK) {
        sink = stringSink.GetString();
    }
    return status;
}

class DefaultKeyStoreListener : public KeyStoreListener {

  public:

    DefaultKeyStoreListener(const qcc::String& application, const char* fname) {
        if (fname) {
            fileName = GetHomeDir() + fname;
        } else {
            fileName = GetHomeDir() + "/.alljoyn_keystore/" + application;
        }
    }

    QStatus LoadRequest(KeyStore& keyStore) {
        QStatus status;
        /* Try to load the keystore */
        {
            FileSource source(fileName);
            if (source.IsValid()) {
                source.Lock(true);
                status = keyStore.Pull(source, fileName);
                if (status == ER_OK) {
                    QCC_DbgHLPrintf(("Read key store from %s", fileName.c_str()));
                }
                source.Unlock();
                return status;
            }
        }
        /* Create an empty keystore */
        {
            FileSink sink(fileName, FileSink::PRIVATE);
            if (!sink.IsValid()) {
                status = ER_BUS_WRITE_ERROR;
                QCC_LogError(status, ("Cannot initialize key store %s", fileName.c_str()));
                return status;
            }
        }
        /* Load the empty keystore */
        {
            FileSource source(fileName);
            if (source.IsValid()) {
                source.Lock(true);
                status = keyStore.Pull(source, fileName);
                if (status == ER_OK) {
                    QCC_DbgHLPrintf(("Initialized key store %s", fileName.c_str()));
                } else {
                    QCC_LogError(status, ("Failed to initialize key store %s", fileName.c_str()));
                }
                source.Unlock();
            } else {
                status = ER_BUS_READ_ERROR;
            }
            return status;
        }
    }

    QStatus StoreRequest(KeyStore& keyStore) {
        QStatus status;
        FileSink sink(fileName, FileSink::PRIVATE);
        if (sink.IsValid()) {
            sink.Lock(true);
            status = keyStore.Push(sink);
            if (status == ER_OK) {
                QCC_DbgHLPrintf(("Wrote key store to %s", fileName.c_str()));
            }
            sink.Unlock();
        } else {
            status = ER_BUS_WRITE_ERROR;
            QCC_LogError(status, ("Cannot write key store to %s", fileName.c_str()));
        }
        return status;
    }

  private:

    qcc::String fileName;

};

KeyStore::KeyStore(const qcc::String& application) :
    application(application),
    storeState(UNAVAILABLE),
    keys(new KeyMap),
    defaultListener(NULL),
    listener(NULL),
    thisGuid(),
    keyStoreKey(NULL),
    shared(false),
    stored(NULL),
    loaded(NULL)
{
}

KeyStore::~KeyStore()
{
    /* Unblock thread that might be waiting for a store to complete */
    lock.Lock();
    if (stored) {
        stored->SetEvent();
        lock.Unlock();
        while (stored) {
            qcc::Sleep(1);
        }
        lock.Lock();
    }
    /* Unblock thread that might be waiting for a load to complete */
    if (loaded) {
        loaded->SetEvent();
        lock.Unlock();
        while (loaded) {
            qcc::Sleep(1);
        }
        lock.Lock();
    }
    lock.Unlock();
    delete defaultListener;
    delete keyStoreKey;
    delete keys;
}

QStatus KeyStore::SetListener(KeyStoreListener& listener)
{
    if (this->listener != NULL) {
        return ER_BUS_LISTENER_ALREADY_SET;
    } else {
        this->listener = &listener;
        return ER_OK;
    }
}

QStatus KeyStore::Init(const char* fileName, bool isShared)
{
    if (storeState == UNAVAILABLE) {
        if (listener == NULL) {
            listener = defaultListener = new DefaultKeyStoreListener(application, fileName);
        }
        shared = isShared;
        return Load();
    } else {
        return ER_FAIL;
    }
}

QStatus KeyStore::Store()
{
    QStatus status = ER_OK;

    /* Cannot store if never loaded */
    if (storeState == UNAVAILABLE) {
        return ER_BUS_KEYSTORE_NOT_LOADED;
    }
    /* Don't store if not modified */
    if (storeState == MODIFIED) {

        lock.Lock();
        EraseExpiredKeys();

        /* Reload to merge keystore changes before storing */
        if (revision > 0) {
            lock.Unlock();
            status = Reload();
            lock.Lock();
        }
        if (status == ER_OK) {
            stored = new Event();
            lock.Unlock();
            status = listener->StoreRequest(*this);
            if (status == ER_OK) {
                status = Event::Wait(*stored);
            }
            lock.Lock();
            delete stored;
            stored = NULL;
            /* Done tracking deletions */
            deletions.clear();
        }
        lock.Unlock();
    }
    return status;
}

QStatus KeyStore::Load()
{
    QStatus status;
    lock.Lock();
    keys->clear();
    storeState = UNAVAILABLE;
    loaded = new Event();
    lock.Unlock();
    status = listener->LoadRequest(*this);
    if (status == ER_OK) {
        status = Event::Wait(*loaded);
    }
    lock.Lock();
    delete loaded;
    loaded = NULL;
    lock.Unlock();
    return status;
}

size_t KeyStore::EraseExpiredKeys()
{
    size_t count = 0;
    KeyMap::iterator it = keys->begin();
    while (it != keys->end()) {
        KeyMap::iterator current = it++;
        if (current->second.key.HasExpired()) {
            QCC_DbgPrintf(("Deleting expired key for GUID %s", current->first.ToString().c_str()));
            keys->erase(current);
            ++count;
        }
    }
    return count;
}

QStatus KeyStore::Pull(Source& source, const qcc::String& password)
{
    QCC_DbgPrintf(("KeyStore::Pull"));

    /* Don't load if already loaded */
    if (storeState != UNAVAILABLE) {
        return ER_OK;
    }

    lock.Lock();

    uint8_t guidBuf[qcc::GUID::SIZE];
    size_t pulled;
    size_t len = 0;
    uint16_t version;
    KeyBlob nonce;

    /* Pull and check the key store version */
    QStatus status = source.PullBytes(&version, sizeof(version), pulled);
    if ((status == ER_OK) && (version != KeyStoreVersion)) {
        /* We can still read the back version */
        if (version != BackVersion) {
            status = ER_BUS_KEYSTORE_VERSION_MISMATCH;
        }
        QCC_LogError(status, ("Keystore has wrong version expected %d got %d", KeyStoreVersion, version));
    }
    if (version == BackVersion) {
        revision = 0;
    } else {
        /* Pull the revision number */
        if (status == ER_OK) {
            status = source.PullBytes(&revision, sizeof(revision), pulled);
        }
    }
    /* Pull the application GUID */
    if (status == ER_OK) {
        status = source.PullBytes(guidBuf, qcc::GUID::SIZE, pulled);
        thisGuid.SetBytes(guidBuf);
    }

    /* This is the only chance to generate the key store key */
    keyStoreKey = new KeyBlob(password + GetGuid(), Crypto_AES::AES128_SIZE, KeyBlob::AES);

    /* Allow for an uninitialized (empty) key store */
    if (status == ER_NONE) {
        keys->clear();
        storeState = MODIFIED;
        revision = 0;
        status = ER_OK;
        goto ExitPull;
    }
    if (status != ER_OK) {
        goto ExitPull;
    }
    QCC_DbgPrintf(("KeyStore::Pull (revision %d)", revision));
    /* Pull the nonce */
    status = nonce.Load(source);
    if (status != ER_OK) {
        goto ExitPull;
    }
    /* Get length of the encrypted keys */
    status = source.PullBytes(&len, sizeof(len), pulled);
    if (status != ER_OK) {
        goto ExitPull;
    }
    /* Sanity check on the length */
    if (len > 64000) {
        status = ER_BUS_CORRUPT_KEYSTORE;
        goto ExitPull;
    }
    if (len > 0) {
        uint8_t* data = NULL;
        /*
         * Pull the encrypted keys.
         */
        data = new uint8_t[len];
        status = source.PullBytes(data, len, pulled);
        if (pulled != len) {
            status = ER_BUS_CORRUPT_KEYSTORE;
        }
        if (status == ER_OK) {
            /*
             * Decrypt the key store.
             */
            Crypto_AES aes(*keyStoreKey, Crypto_AES::ENCRYPT);
            status = aes.Decrypt_CCM(data, data, len, nonce, NULL, 0, 16);
            /*
             * Unpack the guid/key pairs from an intermediate string source.
             */
            StringSource strSource(data, len);
            while (status == ER_OK) {
                uint32_t rev;
                if (version == BackVersion) {
                    rev = 0;
                } else {
                    status = strSource.PullBytes(&rev, sizeof(rev), pulled);
                }
                if (status == ER_OK) {
                    status = strSource.PullBytes(guidBuf, qcc::GUID::SIZE, pulled);
                }
                if (status == ER_OK) {
                    qcc::GUID guid;
                    guid.SetBytes(guidBuf);
                    KeyRecord& keyRec = (*keys)[guid];
                    keyRec.revision = rev;
                    status = keyRec.key.Load(strSource);
                    QCC_DbgPrintf(("KeyStore::Pull rev:%d GUID %s %s", rev, QCC_StatusText(status), guid.ToString().c_str()));
                }
            }
            if (status == ER_NONE) {
                status = ER_OK;
            }
        }
        delete [] data;
    }
    if (status != ER_OK) {
        goto ExitPull;
    }
    if (EraseExpiredKeys()) {
        storeState = MODIFIED;
    } else {
        storeState = LOADED;
    }

ExitPull:

    if (status != ER_OK) {
        keys->clear();
        storeState = MODIFIED;
    }
    if (loaded) {
        loaded->SetEvent();
    }
    lock.Unlock();
    return status;
}

QStatus KeyStore::Clear()
{
    if (storeState == UNAVAILABLE) {
        return ER_BUS_KEYSTORE_NOT_LOADED;
    }
    lock.Lock();
    keys->clear();
    storeState = MODIFIED;
    revision = 0;
    deletions.clear();
    lock.Unlock();
    listener->StoreRequest(*this);
    return ER_OK;
}

QStatus KeyStore::Reload()
{
    QCC_DbgHLPrintf(("KeyStore::Reload"));

    /*
     * Cannot reload if the key store has never been loaded
     */
    if (storeState == UNAVAILABLE) {
        return ER_BUS_KEYSTORE_NOT_LOADED;
    }
    /*
     * Reload is defined to be a no-op for non-shared key stores
     */
    if (!shared) {
        return ER_OK;
    }

    lock.Lock();

    QStatus status;
    uint32_t currentRevision = revision;
    KeyMap* currentKeys = keys;
    keys = new KeyMap();

    /*
     * Load the keys so we can check for changes and merge if needed
     */
    lock.Unlock();
    status = Load();
    lock.Lock();

    /*
     * Check if key store has been changed since we last touched it.
     */
    if ((status == ER_OK) && (revision > currentRevision)) {
        QCC_DbgHLPrintf(("KeyStore::Reload merging changes"));
        KeyMap::iterator it;
        /*
         * Handle deletions
         */
        std::set<qcc::GUID>::iterator itDel;
        for (itDel = deletions.begin(); itDel != deletions.end(); ++itDel) {
            it = keys->find(*itDel);
            if ((it != keys->end()) && (it->second.revision <= currentRevision)) {
                QCC_DbgPrintf(("KeyStore::Reload deleting %s", itDel->ToString().c_str()));
                keys->erase(*itDel);
            }
        }
        /*
         * Handle additions and updates
         */
        for (it = currentKeys->begin(); it != currentKeys->end(); ++it) {
            if (it->second.revision > currentRevision) {
                QCC_DbgPrintf(("KeyStore::Reload added rev:%d %s", it->second.revision, it->first.ToString().c_str()));
                if ((*keys)[it->first].revision > currentRevision) {
                    /*
                     * In case of a merge conflict go with the key that is currently stored
                     */
                    QCC_DbgPrintf(("KeyStore::Reload merge conflict rev:%d %s", it->second.revision, it->first.ToString().c_str()));
                } else {
                    (*keys)[it->first] = it->second;
                    QCC_DbgPrintf(("KeyStore::Reload merging %s", it->first.ToString().c_str()));
                }
            }
        }
        delete currentKeys;
        EraseExpiredKeys();
    } else {
        /*
         * Restore state
         */
        KeyMap* goner = keys;
        keys = currentKeys;
        delete goner;
        revision = currentRevision;
    }

    lock.Unlock();

    return status;
}

QStatus KeyStore::Push(Sink& sink)
{
    size_t pushed;
    QStatus status = ER_OK;

    QCC_DbgHLPrintf(("KeyStore::Push (revision %d)", revision + 1));

    lock.Lock();

    /*
     * Pack the keys into an intermediate string sink.
     */
    StringSink strSink;
    KeyMap::iterator it;
    for (it = keys->begin(); it != keys->end(); ++it) {
        strSink.PushBytes(&it->second.revision, sizeof(revision), pushed);
        strSink.PushBytes(it->first.GetBytes(), qcc::GUID::SIZE, pushed);
        it->second.key.Store(strSink);
        QCC_DbgPrintf(("KeyStore::Push rev:%d GUID %s", it->second.revision, it->first.ToString().c_str()));
    }
    size_t keysLen = strSink.GetString().size();
    KeyBlob nonce;
    nonce.Rand(16, KeyBlob::GENERIC);
    /*
     * First two bytes are the version number.
     */
    status = sink.PushBytes(&KeyStoreVersion, sizeof(KeyStoreVersion), pushed);
    if (status != ER_OK) {
        goto ExitPush;
    }
    /*
     * Second two bytes are the key store revision number. The revision number is incremented each
     * time the key store is stored.
     */
    ++revision;
    status = sink.PushBytes(&revision, sizeof(revision), pushed);
    if (status != ER_OK) {
        goto ExitPush;
    }
    /*
     * Store the GUID and a random nonce.
     */
    if (status == ER_OK) {
        status = sink.PushBytes(thisGuid.GetBytes(), qcc::GUID::SIZE, pushed);
        if (status == ER_OK) {
            status = nonce.Store(sink);
        }
    }
    if (status != ER_OK) {
        goto ExitPush;
    }
    if (keysLen > 0) {
        /*
         * Encrypt keys.
         */
        uint8_t* keysData = new uint8_t[keysLen + 16];
        Crypto_AES aes(*keyStoreKey, Crypto_AES::ENCRYPT);
        status = aes.Encrypt_CCM(strSink.GetString().data(), keysData, keysLen, nonce, NULL, 0, 16);
        /* Store the length of the encrypted keys */
        if (status == ER_OK) {
            status = sink.PushBytes(&keysLen, sizeof(keysLen), pushed);
        }
        /* Store the encrypted keys */
        if (status == ER_OK) {
            status = sink.PushBytes(keysData, keysLen, pushed);
        }
        delete [] keysData;
    } else {
        status = sink.PushBytes(&keysLen, sizeof(keysLen), pushed);
    }
    if (status != ER_OK) {
        goto ExitPush;
    }
    storeState = LOADED;

ExitPush:

    if (stored) {
        stored->SetEvent();
    }
    lock.Unlock();
    return status;
}

QStatus KeyStore::GetKey(const qcc::GUID& guid, KeyBlob& key)
{
    if (storeState == UNAVAILABLE) {
        return ER_BUS_KEYSTORE_NOT_LOADED;
    }
    QStatus status;
    lock.Lock();
    QCC_DbgPrintf(("KeyStore::GetKey %s", guid.ToString().c_str()));
    if (keys->find(guid) != keys->end()) {
        key = (*keys)[guid].key;
        status = ER_OK;
    } else {
        status = ER_BUS_KEY_UNAVAILABLE;
    }
    lock.Unlock();
    return status;
}

bool KeyStore::HasKey(const qcc::GUID& guid)
{
    if (storeState == UNAVAILABLE) {
        return false;
    }
    bool hasKey;
    lock.Lock();
    hasKey = keys->count(guid) != 0;
    lock.Unlock();
    return hasKey;
}

QStatus KeyStore::AddKey(const qcc::GUID& guid, const KeyBlob& key)
{
    if (storeState == UNAVAILABLE) {
        return ER_BUS_KEYSTORE_NOT_LOADED;
    }
    lock.Lock();
    QCC_DbgPrintf(("KeyStore::AddKey %s", guid.ToString().c_str()));
    KeyRecord& keyRec = (*keys)[guid];
    keyRec.revision = revision + 1;
    keyRec.key = key;
    storeState = MODIFIED;
    deletions.erase(guid);
    lock.Unlock();
    return ER_OK;
}

QStatus KeyStore::DelKey(const qcc::GUID& guid)
{
    if (storeState == UNAVAILABLE) {
        return ER_BUS_KEYSTORE_NOT_LOADED;
    }
    lock.Lock();
    QCC_DbgPrintf(("KeyStore::DelKey %s", guid.ToString().c_str()));
    keys->erase(guid);
    storeState = MODIFIED;
    deletions.insert(guid);
    lock.Unlock();
    listener->StoreRequest(*this);
    return ER_OK;
}


}
