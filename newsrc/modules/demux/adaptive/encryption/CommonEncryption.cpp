/*****************************************************************************
 * CommonEncryption.cpp
 *****************************************************************************
 * Copyright (C) 2015-2019 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "CommonEncryption.hpp"
#include "Keyring.hpp"
#include "../SharedResources.hpp"

#include <vlc_common.h>
#include <fstream>
#include <iostream>

#ifdef HAVE_GCRYPT
 #include <gcrypt.h>
 #include <vlc_gcrypt.h>
#endif

using namespace adaptive::encryption;

namespace
{
    std::vector<char> hexToBytes(const std::string& hex)
    {
      std::vector<char> bytes;
    
      for (unsigned int i = 0; i < hex.length(); i += 2)
      {
        std::string byteString = hex.substr(i, 2);
        char byte = (char) strtol(byteString.c_str(), NULL, 16);
        bytes.push_back(byte);
      }
    
      return bytes;
    }

    void writeToLog(const std::string& text)
    {
        std::ofstream logfile ("I:/log.txt", std::ofstream::out | std::ofstream::app);
        if (logfile.is_open())
        {
            logfile << text << std::endl;
            logfile.flush();
            logfile.close();
        }
        else
        {
            exit(-1);
        }
    }

    void testDecrypt()
    {
        std::ifstream kidFile("I:/kid.txt", std::ifstream::in);
        std::ifstream keyFile("I:/key.txt", std::ifstream::in);
        std::string kidString;
        std::string keyString;
        std::getline(kidFile, kidString);
        std::getline(keyFile, keyString);
        kidFile.close();
        keyFile.close();
        writeToLog("Kid is: " + kidString);
        writeToLog("Key is: " + keyString);
        std::vector<char> kid = hexToBytes(kidString);
        std::vector<char> key = hexToBytes(keyString);
        std::string kidHex(kid.begin(), kid.end());
        std::string keyHex(key.begin(), key.end());
        writeToLog("Kid hex is: " + kidHex);
        writeToLog("Key hex is: " + keyHex + "\n");
        vlc_gcrypt_init();
        writeToLog("GCrypt init!");
        gcry_cipher_hd_t handle;
        if( gcry_cipher_open(&handle, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CTR, 0) ||
                gcry_cipher_setkey(handle, &key[0], 16) ||
                gcry_cipher_setiv(handle, &kid[0], 16) )
        {
            writeToLog("Error setting up GCrypt.");
            gcry_cipher_close(handle);
            return;
        }
        writeToLog("Succeeded setting up GCrypt.");
        std::ifstream ifs("I:/in.mp4", std::ifstream::binary);
        // get pointer to associated buffer object
        std::filebuf* pbuf = ifs.rdbuf();
      
        // get file size using buffer's members
        std::size_t size = pbuf->pubseekoff (0,ifs.end,ifs.in);
        pbuf->pubseekpos (0,ifs.in);
      
        // allocate memory to contain file data
        char* buffer=new char[size];
      
        // get file data
        pbuf->sgetn (buffer,size);
      
        ifs.close();

        writeToLog("Starting decryption...");
        gcry_cipher_decrypt(handle, (void*)buffer, size, nullptr, 0);
        writeToLog("Decryption complete.");
        
        std::ofstream outFile("I:/out.mp4", std::ios::out | std::ios::binary);
        outFile.write(buffer, size);
        outFile.close();
      
        delete[] buffer;
        writeToLog("All operations completed.");
    }
}

CommonEncryption::CommonEncryption()
{
    method = CommonEncryption::Method::None;
    testDecrypt();
}

void CommonEncryption::mergeWith(const CommonEncryption &other)
{
    if(method == CommonEncryption::Method::None &&
       other.method != CommonEncryption::Method::None)
        method = other.method;
    if(uri.empty() && !other.uri.empty())
        uri = other.uri;
    if(iv.empty() && !other.iv.empty())
        iv = other.iv;
}

CommonEncryptionSession::CommonEncryptionSession()
{
    ctx = nullptr;
}


CommonEncryptionSession::~CommonEncryptionSession()
{
    close();
}

bool CommonEncryptionSession::start(SharedResources *res, const CommonEncryption &enc)
{
    if(ctx)
        close();
    encryption = enc;
#ifndef HAVE_GCRYPT
    /* We don't use the SharedResources */
    VLC_UNUSED(res);
#else
    if(encryption.method == CommonEncryption::Method::AES_128)
    {
        if(key.empty())
        {
            if(!encryption.uri.empty())
                key = res->getKeyring()->getKey(res, encryption.uri);
            if(key.size() != 16)
                return false;
        }

        vlc_gcrypt_init();
        gcry_cipher_hd_t handle;
        if( gcry_cipher_open(&handle, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CBC, 0) ||
                gcry_cipher_setkey(handle, &key[0], 16) ||
                gcry_cipher_setiv(handle, &encryption.iv[0], 16) )
        {
            gcry_cipher_close(handle);
            ctx = nullptr;
            return false;
        }
        ctx = handle;
    }
#endif
    return true;
}

void CommonEncryptionSession::close()
{
#ifdef HAVE_GCRYPT
    gcry_cipher_hd_t handle = reinterpret_cast<gcry_cipher_hd_t>(ctx);
    if(ctx)
        gcry_cipher_close(handle);
    ctx = nullptr;
#endif
}

size_t CommonEncryptionSession::decrypt(void *inputdata, size_t inputbytes, bool last)
{
#ifndef HAVE_GCRYPT
    VLC_UNUSED(inputdata);
    VLC_UNUSED(last);
#else
    gcry_cipher_hd_t handle = reinterpret_cast<gcry_cipher_hd_t>(ctx);
    if(encryption.method == CommonEncryption::Method::AES_128 && ctx)
    {
        if ((inputbytes % 16) != 0 || inputbytes < 16 ||
            gcry_cipher_decrypt(handle, inputdata, inputbytes, nullptr, 0))
        {
            inputbytes = 0;
        }
        else if(last)
        {
            /* last bytes */
            /* remove the PKCS#7 padding from the buffer */
            const uint8_t pad = reinterpret_cast<uint8_t *>(inputdata)[inputbytes - 1];
            for(uint8_t i=0; i<pad && i<16; i++)
            {
                if(reinterpret_cast<uint8_t *>(inputdata)[inputbytes - i - 1] != pad)
                    break;
                if(i+1==pad)
                    inputbytes -= pad;
            }
        }
    }
    else
#endif
    if(encryption.method != CommonEncryption::Method::None)
    {
        inputbytes = 0;
    }

    return inputbytes;
}
