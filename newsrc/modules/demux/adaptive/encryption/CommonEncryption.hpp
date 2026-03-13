/*****************************************************************************
 * CommonEncryption.hpp
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
#ifndef COMMONENCRYPTION_H
#define COMMONENCRYPTION_H

#include <vector>
#include <string>

namespace adaptive
{
    class SharedResources;

    namespace encryption
    {
        class CommonEncryption
        {
            public:
                CommonEncryption();
                void mergeWith(const CommonEncryption &);
                enum class Method
                {
                    None,
                    AES_128,
                    AES_128_CTR,
                    AES_Sample,
                } method;
                std::string uri;
                std::vector<unsigned char> iv;
        };

        class CommonEncryptionSession
        {
            public:
                CommonEncryptionSession();
                ~CommonEncryptionSession();

                bool start(SharedResources *, const CommonEncryption &);
                void close();
                size_t decrypt(void *, size_t, bool);
                CommonEncryption::Method getEncryptionMethod() const { return encryption.method; }

            private:
                std::vector<unsigned char> key;
                std::string keyCTR;
                CommonEncryption encryption;
                void *ctx;

                enum ForceSyncMode {
                    AP4_FRAGMENTER_FORCE_SYNC_MODE_NONE,
                    AP4_FRAGMENTER_FORCE_SYNC_MODE_AUTO,
                    AP4_FRAGMENTER_FORCE_SYNC_MODE_ALL
                };

                struct _Options {
                    bool          trim;
                    bool          no_tfdt;
                    double        tfdt_start;
                    unsigned int  sequence_number_start;
                    ForceSyncMode force_i_frame_sync;
                    bool          no_zero_elst;
                } Options;

                static constexpr const unsigned int AP4_FRAGMENTER_DEFAULT_FRAGMENT_DURATION   = 2000;
                static constexpr const unsigned int AP4_FRAGMENTER_MAX_AUTO_FRAGMENT_DURATION  = 40000;
                static constexpr const unsigned int AP4_FRAGMENTER_OUTPUT_MOVIE_TIMESCALE      = 1000;
        };
    }
}

#endif
