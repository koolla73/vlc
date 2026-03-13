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

#include "Ap4.h"
#include "CommonEncryption.hpp"
#include "Keyring.hpp"
#include "../SharedResources.hpp"

#include <vlc_common.h>
#include <vlc_strings.h>

#ifdef HAVE_GCRYPT
 #include <gcrypt.h>
 #include <vlc_gcrypt.h>
#endif

using namespace adaptive::encryption;

class SampleArray {
public:
    SampleArray(AP4_Track* track) :
        m_Track(track) {
        m_SampleCount = m_Track->GetSampleCount();
        if (m_SampleCount) {
            m_ForcedSync = new bool[m_SampleCount];
            for (unsigned int i=0; i<m_SampleCount; i++) {
                m_ForcedSync[i] = false;
            }
        } else {
            m_ForcedSync = NULL;
        }
    }
    virtual ~SampleArray() {
        delete[] m_ForcedSync;
    }

    virtual AP4_Cardinal GetSampleCount() {
        return m_SampleCount;
    }
    virtual AP4_Result GetSample(AP4_Ordinal index, AP4_Sample& sample) {
        AP4_Result result = m_Track->GetSample(index, sample);
        if (AP4_SUCCEEDED(result)) {
            if (m_ForcedSync[index]) {
                sample.SetSync(true);
            }
        }
        return result;
    }
    virtual AP4_Result AddSample(AP4_Sample& /*sample*/) {
        return AP4_ERROR_NOT_SUPPORTED;
    }
    virtual void ForceSync(AP4_Ordinal index) {
        if (index < m_SampleCount) {
            m_ForcedSync[index] = true;
        }
    }
    
protected:
    AP4_Track*   m_Track;
    AP4_Cardinal m_SampleCount;
    bool*        m_ForcedSync;
};

class CachedSampleArray : public SampleArray {
public:
    CachedSampleArray(AP4_Track* track) :
        SampleArray(track) {}

    virtual AP4_Cardinal GetSampleCount() {
        return m_Samples.ItemCount();
    }
    virtual AP4_Result GetSample(AP4_Ordinal index, AP4_Sample& sample) {
        if (index >= m_Samples.ItemCount()) {
            return AP4_ERROR_OUT_OF_RANGE;
        } else {
            sample = m_Samples[index];
            return AP4_SUCCESS;
        }
    }
    virtual AP4_Result AddSample(AP4_Sample& sample) {
        return m_Samples.Append(sample);
    }
    
protected:
    AP4_Array<AP4_Sample> m_Samples;
};

class TrackCursor
{
public:
    TrackCursor(AP4_Track* track, SampleArray* samples);
    ~TrackCursor();
    
    AP4_Result    Init();
    AP4_Result    SetSampleIndex(AP4_Ordinal sample_index);
    
    AP4_Track*    m_Track;
    SampleArray*  m_Samples;
    AP4_Ordinal   m_SampleIndex;
    AP4_Ordinal   m_FragmentIndex;
    AP4_Sample    m_Sample;
    AP4_UI64      m_Timestamp;
    AP4_UI64      m_UnscaledTimestamp;
    bool          m_Eos;
    AP4_TfraAtom* m_Tfra;
};

TrackCursor::TrackCursor(AP4_Track* track, SampleArray* samples) :
    m_Track(track),
    m_Samples(samples),
    m_SampleIndex(0),
    m_FragmentIndex(0),
    m_Timestamp(0),
    m_UnscaledTimestamp(0),
    m_Eos(false),
    m_Tfra(new AP4_TfraAtom(0))
{
}

TrackCursor::~TrackCursor()
{
    delete m_Tfra;
    delete m_Samples;
}

AP4_Result
TrackCursor::Init()
{
    return m_Samples->GetSample(0, m_Sample);
}

AP4_Result
TrackCursor::SetSampleIndex(AP4_Ordinal sample_index)
{
    m_SampleIndex = sample_index;
    
    // check if we're at the end
    if (sample_index >= m_Samples->GetSampleCount()) {
        AP4_UI64 end_dts = m_Sample.GetDts()+m_Sample.GetDuration();
        m_Sample.Reset();
        m_Sample.SetDts(end_dts);
        m_Eos = true;
    } else {
        return m_Samples->GetSample(m_SampleIndex, m_Sample);
    }
    
    return AP4_SUCCESS;
}

class FragmentInfo {
public:
    FragmentInfo(SampleArray* samples, AP4_TfraAtom* tfra, AP4_UI64 timestamp, AP4_ContainerAtom* moof) :
        m_Samples(samples),
        m_Tfra(tfra),
        m_Timestamp(timestamp),
        m_Duration(0),
        m_Moof(moof),
        m_MoofPosition(0),
        m_MdatSize(0) {}
    
    SampleArray*        m_Samples;
    AP4_TfraAtom*       m_Tfra;
    AP4_UI64            m_Timestamp;
    AP4_UI32            m_Duration;
    AP4_Array<AP4_UI32> m_SampleIndexes;
    AP4_ContainerAtom*  m_Moof;
    AP4_Position        m_MoofPosition;
    AP4_UI32            m_MdatSize;
};

class IndexedSegmentInfo {
public:
    IndexedSegmentInfo() : m_Size(0), m_Duration(0) {}
    AP4_UI32 m_Size;
    AP4_UI32 m_Duration;
};

static void Fragment(AP4_File&                input_file,
         AP4_MemoryByteStream&          output_stream,
         AP4_Array<TrackCursor*>& cursors,
         AP4_UI32                 fragment_duration,
         AP4_UI32                 timescale,
         bool                     create_segment_index,
         bool                     copy_udta,
         bool                     trun_version_one)
{
    AP4_List<FragmentInfo>       fragments;
    AP4_List<IndexedSegmentInfo> indexed_segments;
    IndexedSegmentInfo*          current_indexed_segment = NULL;
    AP4_Result                   result;
    
    // get the movie
    AP4_Movie* input_movie = input_file.GetMovie();
    if (input_movie == NULL) {
        return;
    }

    // create the output file object
    AP4_UI64 creation_time = 0;
    time_t now = time(NULL);
    if (now != (time_t)-1) {
        // adjust the time based on the MPEG time origin
        creation_time = (AP4_UI64)now + 0x7C25B080;
    }
    AP4_Movie* output_movie = new AP4_Movie(AP4_FRAGMENTER_OUTPUT_MOVIE_TIMESCALE, 0, creation_time, creation_time);
    
    // create an mvex container
    AP4_ContainerAtom* mvex = new AP4_ContainerAtom(AP4_ATOM_TYPE_MVEX);
    AP4_MehdAtom*      mehd = new AP4_MehdAtom(0);
    mvex->AddChild(mehd);
    
    // add an output track for each track in the input file
    for (unsigned int i=0; i<cursors.ItemCount(); i++) {
        AP4_Track* track = cursors[i]->m_Track;
        
        result = cursors[i]->Init();
        if (AP4_FAILED(result)) {
            return;
        }

        // create a sample table (with no samples) to hold the sample description
        AP4_SyntheticSampleTable* sample_table = new AP4_SyntheticSampleTable();
        for (unsigned int j=0; j<track->GetSampleDescriptionCount(); j++) {
            AP4_SampleDescription* sample_description = track->GetSampleDescription(j);
            sample_table->AddSampleDescription(sample_description, false);
        }
        
        // create the track
        AP4_Track* output_track = new AP4_Track(sample_table,
                                                track->GetId(),
                                                timescale?timescale:AP4_FRAGMENTER_OUTPUT_MOVIE_TIMESCALE,
                                                AP4_ConvertTime(track->GetDuration(),
                                                                input_movie->GetTimeScale(),
                                                                timescale?timescale:AP4_FRAGMENTER_OUTPUT_MOVIE_TIMESCALE),
                                                timescale?timescale:track->GetMediaTimeScale(),
                                                0,//track->GetMediaDuration(),
                                                track);
        
        // add an edit list if needed
        if (!trun_version_one) {
          if (const AP4_TrakAtom* trak = track->GetTrakAtom()) {
              AP4_ContainerAtom* edts = AP4_DYNAMIC_CAST(AP4_ContainerAtom, trak->GetChild(AP4_ATOM_TYPE_EDTS));
              if (edts) {
                  // create an 'edts' container
                  AP4_ContainerAtom* new_edts = new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
                  
                  // create a new 'edts' for each original 'edts'
                  for (AP4_List<AP4_Atom>::Item* edts_entry = edts->GetChildren().FirstItem();
                       edts_entry;
                       edts_entry = edts_entry->GetNext()) {
                      AP4_ElstAtom* elst = AP4_DYNAMIC_CAST(AP4_ElstAtom, edts_entry->GetData());
                      AP4_ElstAtom* new_elst = new AP4_ElstAtom();
                      
                      // adjust the fields to match the correct timescale
                      for (unsigned int j=0; j<elst->GetEntries().ItemCount(); j++) {
                          AP4_ElstEntry new_elst_entry = elst->GetEntries()[j];
                          if (j == elst->GetEntries().ItemCount() - 1 &&
                              new_elst_entry.m_SegmentDuration == track->GetDuration() &&
                              !Options.no_zero_elst) {
                              // if this is the last entry, make the segment duration 0 (i.e last until the end)
                              // in order to be compliant with the CMAF specification
                              new_elst_entry.m_SegmentDuration = 0;
                          } else {
                              new_elst_entry.m_SegmentDuration = AP4_ConvertTime(new_elst_entry.m_SegmentDuration,
                                                                                 input_movie->GetTimeScale(),
                                                                                 AP4_FRAGMENTER_OUTPUT_MOVIE_TIMESCALE);
                          }
                          if (new_elst_entry.m_MediaTime > 0 && timescale) {
                              new_elst_entry.m_MediaTime = (AP4_SI64)AP4_ConvertTime(new_elst_entry.m_MediaTime,
                                                                                     track->GetMediaTimeScale(),
                                                                                     timescale?timescale:track->GetMediaTimeScale());
                                                                                 
                          }
                          new_elst->AddEntry(new_elst_entry);
                      }
                      
                      // add the 'elst' to the 'edts' container
                      new_edts->AddChild(new_elst);
                  }
                  
                  // add the edit list to the output track (just after the 'tkhd' atom)
                  output_track->UseTrakAtom()->AddChild(new_edts, 1);
              }
          }
        }
        
        // add the track to the output
        output_movie->AddTrack(output_track);
        
        // add a trex entry to the mvex container
        AP4_TrexAtom* trex = new AP4_TrexAtom(track->GetId(),
                                              1,
                                              0,
                                              0,
                                              0);
        mvex->AddChild(trex);
    }
    
    // select the anchor cursor
    TrackCursor* anchor_cursor = NULL;
    if (cursors.ItemCount() == 1) {
        // only one track, that's our anchor
        anchor_cursor = cursors[0];
    }
    if (anchor_cursor == NULL) {
        for (unsigned int i=0; i<cursors.ItemCount(); i++) {
            // use this as the anchor track if it is the first video track
            if (cursors[i]->m_Track->GetType() == AP4_Track::TYPE_VIDEO) {
                anchor_cursor = cursors[i];
                break;
            }
        }
    }
    if (anchor_cursor == NULL) {
        // no video track to anchor with, pick the first audio track
        for (unsigned int i=0; i<cursors.ItemCount(); i++) {
            if (cursors[i]->m_Track->GetType() == AP4_Track::TYPE_AUDIO) {
                anchor_cursor = cursors[i];
                break;
            }
        }
        // no audio track to anchor with, pick the first subtitles track
        for (unsigned int i=0; i<cursors.ItemCount(); i++) {
            if (cursors[i]->m_Track->GetType() == AP4_Track::TYPE_SUBTITLES) {
                anchor_cursor = cursors[i];
                break;
            }
        }
    }
    if (anchor_cursor == NULL) {
        // this should never happen
        return;
    }
    
    // decide which tracks to index and in which order
    TrackCursor* indexed_cursor = anchor_cursor;

    // update the mehd duration
    mehd->SetDuration(output_movie->GetDuration());
    
    // add the mvex container to the moov container
    output_movie->GetMoovAtom()->AddChild(mvex);

    // copy the moov/udta atom to the moov container
    if (copy_udta) {
        AP4_Atom* udta = input_movie->GetMoovAtom()->GetChild(AP4_ATOM_TYPE_UDTA);
        if (udta != NULL) {
            output_movie->GetMoovAtom()->AddChild(udta->Clone());
        }
    }
    
    // compute all the fragments
    unsigned int sequence_number = Options.sequence_number_start;
    for(;;) {
        TrackCursor* cursor = NULL;

        // pick the first track with a fragment index lower than the anchor's
        for (unsigned int i=0; i<cursors.ItemCount(); i++) {
            if (cursors[i]->m_Eos) continue;
            if (cursors[i]->m_FragmentIndex < anchor_cursor->m_FragmentIndex) {
                cursor = cursors[i];
                break;
            }
        }
        
        // check if we found a non-anchor cursor to use
        if (cursor == NULL) {
            // the anchor should be used in this round, check if we can use it
            if (anchor_cursor->m_Eos) {
                // the anchor is done, pick a new anchor unless we need to trim
                anchor_cursor = NULL;
                if (!Options.trim) {
                    for (unsigned int i=0; i<cursors.ItemCount(); i++) {
                        if (cursors[i]->m_Eos) continue;
                        if (anchor_cursor == NULL ||
                            cursors[i]->m_Track->GetType() == AP4_Track::TYPE_VIDEO ||
                            cursors[i]->m_Track->GetType() == AP4_Track::TYPE_AUDIO) {
                            anchor_cursor = cursors[i];
                        }
                    }
                }
            }
            cursor = anchor_cursor;
        }
        if (cursor == NULL) break; // all done
        
        // decide how many samples go into this fragment
        AP4_UI64 target_dts;
        if (cursor == anchor_cursor) {
            // compute the current dts in milliseconds
            AP4_UI64 anchor_dts_ms = AP4_ConvertTime(cursor->m_Sample.GetDts(),
                                                     cursor->m_Track->GetMediaTimeScale(),
                                                     1000);
            // round to the nearest multiple of fragment_duration
            AP4_UI64 anchor_position = (anchor_dts_ms + (fragment_duration/2))/fragment_duration;
            
            // pick the next fragment_duration multiple at our target
            target_dts = AP4_ConvertTime(fragment_duration*(anchor_position+1),
                                         1000,
                                         cursor->m_Track->GetMediaTimeScale());
        } else {
            target_dts = AP4_ConvertTime(anchor_cursor->m_Sample.GetDts(),
                                         anchor_cursor->m_Track->GetMediaTimeScale(),
                                         cursor->m_Track->GetMediaTimeScale());
            if (target_dts <= cursor->m_Sample.GetDts()) {
                // we must be at the end, past the last anchor sample, just use the target duration
                target_dts = AP4_ConvertTime((AP4_UI64)fragment_duration*(cursor->m_FragmentIndex+1),
                                            1000,
                                            cursor->m_Track->GetMediaTimeScale());
                
                if (target_dts <= cursor->m_Sample.GetDts()) {
                    // we're still behind, there may have been an alignment/rounding error, just advance by one segment duration
                    target_dts = cursor->m_Sample.GetDts()+AP4_ConvertTime(fragment_duration,
                                                                           1000,
                                                                           cursor->m_Track->GetMediaTimeScale());
                }
            }
        }

        unsigned int end_sample_index = cursor->m_Samples->GetSampleCount();
        AP4_UI64 smallest_diff = (AP4_UI64)(0xFFFFFFFFFFFFFFFFULL);
        AP4_Sample sample;
        for (unsigned int i=cursor->m_SampleIndex+1; i<=cursor->m_Samples->GetSampleCount(); i++) {
            AP4_UI64 dts;
            if (i < cursor->m_Samples->GetSampleCount()) {
                result = cursor->m_Samples->GetSample(i, sample);
                if (AP4_FAILED(result)) {
                    return;
                }
                if (!sample.IsSync()) continue; // only look for sync samples
                dts = sample.GetDts();
            } else {
                result = cursor->m_Samples->GetSample(i-1, sample);
                if (AP4_FAILED(result)) {
                    return;
                }
                dts = sample.GetDts()+sample.GetDuration();
            }
            AP4_SI64 diff = dts-target_dts;
            AP4_UI64 abs_diff = diff<0?-diff:diff;
            if (abs_diff < smallest_diff) {
                // this sample is the closest to the target so far
                end_sample_index = i;
                smallest_diff = abs_diff;
            }
            if (diff >= 0) {
                // this sample is past the target, it is not going to get any better, stop looking
                break;
            }
        }
        if (cursor->m_Eos) continue;

        // decide which sample description index to use
        // (this is not very sophisticated, we only look at the sample description
        // index of the first sample in the group, which may not be correct. This
        // should be fixed later)
        unsigned int sample_desc_index = cursor->m_Sample.GetDescriptionIndex();

        // set initial flag values
        AP4_UI32 tfhd_flags = AP4_TFHD_FLAG_DEFAULT_BASE_IS_MOOF | AP4_TFHD_FLAG_SAMPLE_DESCRIPTION_INDEX_PRESENT;
        AP4_UI32 trun_flags = AP4_TRUN_FLAG_DATA_OFFSET_PRESENT |
                              AP4_TRUN_FLAG_SAMPLE_SIZE_PRESENT;
        AP4_UI32 sync_sample_flags = 0;
        AP4_UI32 non_sync_sample_flags = 0x10000; // 0x10000 -> sample_is_non_sync
        if (cursor->m_Track->GetType() == AP4_Track::TYPE_VIDEO ||
            (cursor->m_Track->GetSampleDescriptionCount() > 0 && cursor->m_Track->GetSampleDescription(0) &&
             cursor->m_Track->GetSampleDescription(0)->GetFormat() == AP4_SAMPLE_FORMAT_AC_4)) {
            non_sync_sample_flags |= 0x1000000; // sample_depends_on=1 (not I frame)
            sync_sample_flags     |= 0x2000000; // sample_depends_on=2 (I frame)
        }
        
        // setup the moof structure
        AP4_ContainerAtom* moof = new AP4_ContainerAtom(AP4_ATOM_TYPE_MOOF);
        AP4_MfhdAtom* mfhd = new AP4_MfhdAtom(sequence_number++);
        moof->AddChild(mfhd);
        AP4_ContainerAtom* traf = new AP4_ContainerAtom(AP4_ATOM_TYPE_TRAF);
        AP4_TfhdAtom* tfhd = new AP4_TfhdAtom(tfhd_flags,
                                              cursor->m_Track->GetId(),
                                              0,
                                              sample_desc_index+1,
                                              0,
                                              0,
                                              0);
        traf->AddChild(tfhd);
        if (!Options.no_tfdt) {
            AP4_TfdtAtom* tfdt = new AP4_TfdtAtom(1, cursor->m_Timestamp + (AP4_UI64)(Options.tfdt_start * (double)cursor->m_Track->GetMediaTimeScale()));
            traf->AddChild(tfdt);
        }
                                              
        // create the `trun` and `traf` atoms
        AP4_TrunAtom* trun = new AP4_TrunAtom(trun_flags, 0, 0);
        unsigned int initial_offset = 0;
        if (trun_version_one) {
            trun->SetVersion(1);
            initial_offset = cursor->m_Sample.GetCtsDelta();
        }
        traf->AddChild(trun);
        moof->AddChild(traf);
        
        // create a new FragmentInfo object to store the fragment details
        FragmentInfo* fragment = new FragmentInfo(cursor->m_Samples, cursor->m_Tfra, cursor->m_Timestamp, moof);
        fragments.Add(fragment);
        
        // add samples to the fragment
        unsigned int sample_count = 0;
        AP4_Array<AP4_TrunAtom::Entry> trun_entries;
        fragment->m_MdatSize = AP4_ATOM_HEADER_SIZE;
        AP4_UI32 constant_sample_duration = 0;
        bool all_sample_durations_equal = true;
        bool all_samples_are_sync = true;
        bool only_first_sample_is_sync = true;
        for (;;) {
            // if we have one non-zero CTS delta, we'll need to express it
            if (cursor->m_Sample.GetCtsDelta()) {
                trun_flags |= AP4_TRUN_FLAG_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT;
            }
            
            // add one sample
            trun_entries.SetItemCount(sample_count+1);
            AP4_TrunAtom::Entry& trun_entry = trun_entries[sample_count];
            AP4_UI64 next_unscaled_timestamp = cursor->m_UnscaledTimestamp+cursor->m_Sample.GetDuration();
            AP4_UI64 next_scaled_timestamp   = timescale?
                                               AP4_ConvertTime(next_unscaled_timestamp,
                                                               cursor->m_Track->GetMediaTimeScale(),
                                                               timescale):
                                               next_unscaled_timestamp;
            trun_entry.sample_duration                = (AP4_UI32)(next_scaled_timestamp-cursor->m_Timestamp);
            trun_entry.sample_size                    = cursor->m_Sample.GetSize();
            trun_entry.sample_flags                   = cursor->m_Sample.IsSync() ? sync_sample_flags : non_sync_sample_flags;
            trun_entry.sample_composition_time_offset = timescale?
                                                        (AP4_UI32)AP4_ConvertTime(cursor->m_Sample.GetCtsDelta(),
                                                                                  cursor->m_Track->GetMediaTimeScale(),
                                                                                  timescale):
                                                        cursor->m_Sample.GetCtsDelta();
                        
            if (trun->GetVersion() == 1) {
                trun_entry.sample_composition_time_offset -= initial_offset;
            }
            fragment->m_SampleIndexes.SetItemCount(sample_count+1);
            fragment->m_SampleIndexes[sample_count] = cursor->m_SampleIndex;
            fragment->m_MdatSize += trun_entry.sample_size;
            fragment->m_Duration += trun_entry.sample_duration;
            
            // check if the durations are all the same
            if (all_sample_durations_equal) {
                if (constant_sample_duration == 0) {
                    constant_sample_duration = trun_entry.sample_duration;
                } else {
                    if (constant_sample_duration != trun_entry.sample_duration) {
                        all_sample_durations_equal = false;
                    }
                }
            }
            
            // update flag metadata
            if (cursor->m_Sample.IsSync()) {
                if (sample_count) {
                    only_first_sample_is_sync = false;
                }
            } else {
                all_samples_are_sync = false;
            }
            
            // next sample
            cursor->m_UnscaledTimestamp = next_unscaled_timestamp;
            cursor->m_Timestamp         = next_scaled_timestamp;
            result = cursor->SetSampleIndex(cursor->m_SampleIndex+1);
            if (AP4_FAILED(result)) {
                return;
            }
            sample_count++;
            if (cursor->m_Eos) {
                break;
            }
            if (cursor->m_SampleIndex >= end_sample_index) {
                break; // done with this fragment
            }
        }
        
        // update the flags
        if (only_first_sample_is_sync) {
            trun_flags |= AP4_TRUN_FLAG_FIRST_SAMPLE_FLAGS_PRESENT;
            trun->SetFirstSampleFlags(sync_sample_flags);
            tfhd_flags |= AP4_TFHD_FLAG_DEFAULT_SAMPLE_FLAGS_PRESENT;
            tfhd->SetDefaultSampleFlags(non_sync_sample_flags);
        } else {
            if (all_samples_are_sync) {
                tfhd_flags |= AP4_TFHD_FLAG_DEFAULT_SAMPLE_FLAGS_PRESENT;
                tfhd->SetDefaultSampleFlags(0);
            } else {
                trun_flags |= AP4_TRUN_FLAG_SAMPLE_FLAGS_PRESENT;
            }
        }

        if (all_sample_durations_equal) {
            tfhd_flags |= AP4_TFHD_FLAG_DEFAULT_SAMPLE_DURATION_PRESENT;
            tfhd->SetDefaultSampleDuration(constant_sample_duration);
        } else {
            trun_flags |= AP4_TRUN_FLAG_SAMPLE_DURATION_PRESENT;
        }
        
        tfhd->UpdateFlags(tfhd_flags);
        trun->UpdateFlags(trun_flags);
        
        // update moof and children
        trun->SetEntries(trun_entries);
        trun->SetDataOffset((AP4_UI32)moof->GetSize()+AP4_ATOM_HEADER_SIZE);
        
        // keep track of fragments that will be part of the index
        if (cursor == anchor_cursor) {
            // start a new segment
            current_indexed_segment = new IndexedSegmentInfo();
            indexed_segments.Add(current_indexed_segment);
            current_indexed_segment->m_Duration = fragment->m_Duration;
        }
        if (current_indexed_segment) {
            current_indexed_segment->m_Size += (AP4_UI32)(fragment->m_Moof->GetSize()+fragment->m_MdatSize);
        }
        
        // advance the cursor's fragment index
        ++cursor->m_FragmentIndex;
    }
    
    // write the ftyp atom
    AP4_FtypAtom* ftyp = input_file.GetFileType();
    if (ftyp) {
        // keep the existing brand and compatible brands
        AP4_Array<AP4_UI32> compatible_brands;
        compatible_brands.EnsureCapacity(ftyp->GetCompatibleBrands().ItemCount()+1);
        for (unsigned int i=0; i<ftyp->GetCompatibleBrands().ItemCount(); i++) {
            compatible_brands.Append(ftyp->GetCompatibleBrands()[i]);
        }
        
        // add the compatible brand if it is not already there
        if (!ftyp->HasCompatibleBrand(AP4_FILE_BRAND_ISO5)) {
            compatible_brands.Append(AP4_FILE_BRAND_ISO5);
        }

        // create a replacement
        AP4_FtypAtom* new_ftyp = new AP4_FtypAtom(ftyp->GetMajorBrand(),
                                                  ftyp->GetMinorVersion(),
                                                  &compatible_brands[0],
                                                  compatible_brands.ItemCount());
        ftyp = new_ftyp;
    } else {
        AP4_UI32 compat[2] = {
            AP4_FILE_BRAND_ISOM,
            AP4_FILE_BRAND_ISO5
        };
        ftyp = new AP4_FtypAtom(AP4_FTYP_BRAND_MP42, 0, &compat[0], 2);
    }
    ftyp->Write(output_stream);
    delete ftyp;
    
    // write the moov atom
    output_movie->GetMoovAtom()->Write(output_stream);

    // write the (not-yet fully computed) indexes if needed
    AP4_SidxAtom* sidx = NULL;
    AP4_Position  sidx_position = 0;
    output_stream.Tell(sidx_position);
    if (create_segment_index) {
        AP4_UI32 sidx_timescale = timescale ? timescale : indexed_cursor->m_Track->GetMediaTimeScale();
        AP4_UI64 earliest_presentation_time = (AP4_UI64)(Options.tfdt_start * (double)sidx_timescale);
        sidx = new AP4_SidxAtom(indexed_cursor->m_Track->GetId(),
                                sidx_timescale,
                                earliest_presentation_time,
                                0);
        // reserve space for the entries now, but they will be computed and updated later
        sidx->SetReferenceCount(indexed_segments.ItemCount());
        sidx->Write(output_stream);
    }
    
    // write all fragments
    for (AP4_List<FragmentInfo>::Item* item = fragments.FirstItem();
                                       item;
                                       item = item->GetNext()) {
        FragmentInfo* fragment = item->GetData();

        // remember the time and position of this fragment
        output_stream.Tell(fragment->m_MoofPosition);
        fragment->m_Tfra->AddEntry(fragment->m_Timestamp, fragment->m_MoofPosition);
        
        // write the moof
        fragment->m_Moof->Write(output_stream);
        
        // write mdat
        output_stream.WriteUI32(fragment->m_MdatSize);
        output_stream.WriteUI32(AP4_ATOM_TYPE_MDAT);
        AP4_DataBuffer sample_data;
        AP4_Sample     sample;
        for (unsigned int i=0; i<fragment->m_SampleIndexes.ItemCount(); i++) {
            // get the sample
            result = fragment->m_Samples->GetSample(fragment->m_SampleIndexes[i], sample);
            if (AP4_FAILED(result)) {
                return;
            }

            // read the sample data
            result = sample.ReadData(sample_data);
            if (AP4_FAILED(result)) {
                return;
            }
            
            // write the sample data
            result = output_stream.Write(sample_data.GetData(), sample_data.GetDataSize());
            if (AP4_FAILED(result)) {
                return;
            }
        }
    }

    // update the index and re-write it if needed
    if (create_segment_index) {
        unsigned int segment_index = 0;
        AP4_SidxAtom::Reference reference;
        for (AP4_List<IndexedSegmentInfo>::Item* item = indexed_segments.FirstItem();
                                                 item;
                                                 item = item->GetNext()) {
            IndexedSegmentInfo* segment = item->GetData();
            
            // update the sidx entry
            reference.m_ReferencedSize     = segment->m_Size;
            reference.m_SubsegmentDuration = segment->m_Duration;
            reference.m_StartsWithSap      = true;
            reference.m_SapType            = 1;
            sidx->SetReference(segment_index++, reference);
        }
        AP4_Position here = 0;
        output_stream.Tell(here);
        output_stream.Seek(sidx_position);
        sidx->Write(output_stream);
        output_stream.Seek(here);
        delete sidx;
    }
    
    // create an mfra container and write out the index
    AP4_ContainerAtom mfra(AP4_ATOM_TYPE_MFRA);
    for (unsigned int i=0; i<cursors.ItemCount(); i++) {
        mfra.AddChild(cursors[i]->m_Tfra);
        cursors[i]->m_Tfra = NULL;
    }
    AP4_MfroAtom* mfro = new AP4_MfroAtom((AP4_UI32)mfra.GetSize()+16);
    mfra.AddChild(mfro);
    result = mfra.Write(output_stream);
    if (AP4_FAILED(result)) {
        return;
    }
    
    // cleanup
    for (unsigned int i=0; i<cursors.ItemCount(); i++) {
        delete cursors[i];
    }
    for (AP4_List<FragmentInfo>::Item* item = fragments.FirstItem();
                                       item;
                                       item = item->GetNext()) {
        FragmentInfo* fragment = item->GetData();
        delete fragment->m_Moof;
    }
    fragments.DeleteReferences();
    indexed_segments.DeleteReferences();
    delete output_movie;
}

static unsigned int AutoDetectFragmentDuration(TrackCursor* cursor)
{
    AP4_Sample   sample;
    unsigned int sample_count = cursor->m_Samples->GetSampleCount();
    
    // get the first sample as the starting point
    AP4_Result result = cursor->m_Samples->GetSample(0, sample);
    if (AP4_FAILED(result)) {
        return 0;
    }
    if (!sample.IsSync()) {
        return 0;
    }
    
    for (unsigned int interval = 1; interval < sample_count; interval++) {
        bool irregular = false;
        unsigned int sync_count = 0;
        unsigned int i;
        for (i = 0; i < sample_count; i += interval) {
            result = cursor->m_Samples->GetSample(i, sample);
            if (AP4_FAILED(result)) {
                return 0;
            }
            if (!sample.IsSync()) {
                irregular = true;
                break;
            }
            ++sync_count;
        }
        if (sync_count < 1) continue;
        if (!irregular) {
            // found a pattern
            AP4_UI64 duration = sample.GetDts();
            double fps = (double)(interval*(sync_count-1))/((double)duration/(double)cursor->m_Track->GetMediaTimeScale());
            return (unsigned int)(1000.0*(double)interval/fps);
        }
    }
    
    return 0;
}

static unsigned int AutoDetectAudioFragmentDuration(AP4_ByteStream& stream, TrackCursor* cursor)
{
    // remember where we are in the stream
    AP4_Position where = 0;
    stream.Tell(where);
    AP4_LargeSize stream_size = 0;
    stream.GetSize(stream_size);
    AP4_LargeSize bytes_available = stream_size-where;
    
    AP4_UI64  fragment_count = 0;
    AP4_UI32  last_fragment_size = 0;
    AP4_Atom* atom = NULL;
    AP4_DefaultAtomFactory atom_factory;
    while (AP4_SUCCEEDED(atom_factory.CreateAtomFromStream(stream, bytes_available, atom))) {
        if (atom && atom->GetType() == AP4_ATOM_TYPE_MOOF) {
            AP4_ContainerAtom* moof = AP4_DYNAMIC_CAST(AP4_ContainerAtom, atom);
            AP4_TfhdAtom* tfhd = AP4_DYNAMIC_CAST(AP4_TfhdAtom, moof->FindChild("traf/tfhd"));
            if (tfhd && tfhd->GetTrackId() == cursor->m_Track->GetId()) {
                ++fragment_count;
                AP4_TrunAtom* trun = AP4_DYNAMIC_CAST(AP4_TrunAtom, moof->FindChild("traf/trun"));
                if (trun) {
                    last_fragment_size = trun->GetEntries().ItemCount();
                }
            }
        }
        delete atom;
        atom = NULL;
    }
    
    // restore the stream to its original position
    stream.Seek(where);
    
    // decide if we can infer an fragment size
    if (fragment_count == 0 || cursor->m_Samples->GetSampleCount() == 0) {
        return 0;
    }
    // don't count the last fragment if we have more than one
    if (fragment_count > 1 && last_fragment_size) {
        --fragment_count;
    }
    if (fragment_count <= 1 || cursor->m_Samples->GetSampleCount() < last_fragment_size) {
        last_fragment_size = 0;
    }
    AP4_Sample sample;
    AP4_UI64 total_duration = 0;
    for (unsigned int i=0; i<cursor->m_Samples->GetSampleCount()-last_fragment_size; i++) {
        cursor->m_Samples->GetSample(i, sample);
        total_duration += sample.GetDuration();
    }
    return (unsigned int)AP4_ConvertTime(total_duration/fragment_count, cursor->m_Track->GetMediaTimeScale(), 1000);
}

static unsigned int ReadGolomb(AP4_BitStream& bits)
{
    unsigned int leading_zeros = 0;
    while (bits.ReadBit() == 0) {
        leading_zeros++;
    }
    if (leading_zeros) {
        return (1<<leading_zeros)-1+bits.ReadBits(leading_zeros);
    } else {
        return 0;
    }
}

static bool IsIFrame(AP4_Sample& sample, AP4_AvcSampleDescription* avc_desc) {
    AP4_DataBuffer sample_data;
    if (AP4_FAILED(sample.ReadData(sample_data))) {
        return false;
    }

    const unsigned char* data = sample_data.GetData();
    AP4_Size             size = sample_data.GetDataSize();

    while (size >= avc_desc->GetNaluLengthSize()) {
        unsigned int nalu_length = 0;
        if (avc_desc->GetNaluLengthSize() == 1) {
            nalu_length = *data++;
            --size;
        } else if (avc_desc->GetNaluLengthSize() == 2) {
            nalu_length = AP4_BytesToUInt16BE(data);
            data += 2;
            size -= 2;
        } else if (avc_desc->GetNaluLengthSize() == 4) {
            nalu_length = AP4_BytesToUInt32BE(data);
            data += 4;
            size -= 4;
        } else {
            return false;
        }
        if (nalu_length <= size) {
            size -= nalu_length;
        } else {
            size = 0;
        }
        
        switch (*data & 0x1F) {
            case 1: {
                AP4_BitStream bits;
                bits.WriteBytes(data+1, 8);
                ReadGolomb(bits);
                unsigned int slice_type = ReadGolomb(bits);
                if (slice_type == 2 || slice_type == 7) {
                    return true;
                } else {
                    return false; // only show first slice type
                }
            }
            
            case 5: 
                return true;
        }
        
        data += nalu_length;
    }
 
    return false;
}


CommonEncryption::CommonEncryption()
{
    method = CommonEncryption::Method::None;
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

    Options.trim                  = false;
    Options.no_tfdt               = false;
    Options.tfdt_start            = 0.0;
    Options.sequence_number_start = 1;
    Options.force_i_frame_sync    = AP4_FRAGMENTER_FORCE_SYNC_MODE_NONE;
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
    if(encryption.method == CommonEncryption::Method::AES_128_CTR)
    {
        if (keyCTR.empty())
        {
            if(!encryption.uri.empty())
            {
                const std::vector<unsigned char> rawKey = res->getKeyring()->getKey(res, encryption.uri);
                if (rawKey.size() != 16)
                    return false;
                char* hexKey = new char[33];
                vlc_hex_encode_binary(&rawKey[0], rawKey.size(), hexKey);
                keyCTR = std::string(hexKey);
                delete[] hexKey;
            }
            else
                keyCTR = res->getKeyring()->getCustomKey(std::string(encryption.iv.begin(), encryption.iv.end()));
            if(keyCTR.size() != 32)
                return false;
        }
        return true;
    }
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
    if(ctx)
    {
        gcry_cipher_hd_t handle = reinterpret_cast<gcry_cipher_hd_t>(ctx);
        gcry_cipher_close(handle);
    }
    ctx = nullptr;
#endif
}

size_t CommonEncryptionSession::decrypt(void *inputdata, size_t inputbytes, bool last)
{
#ifndef HAVE_GCRYPT
    VLC_UNUSED(inputdata);
    VLC_UNUSED(last);
#else
    if(encryption.method == CommonEncryption::Method::AES_128 && ctx)
    {
        gcry_cipher_hd_t handle = reinterpret_cast<gcry_cipher_hd_t>(ctx);
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
    if(encryption.method == CommonEncryption::Method::AES_128_CTR)
    {
        // Decrypt.
        const std::string kid = std::string(encryption.iv.begin(), encryption.iv.end());
        if (keyCTR.size() != 32 || kid.size() != 32)
            return 0;
        
        unsigned char keyID[16];
        unsigned char decryptionKey[16];
        AP4_ParseHex(kid.c_str(), keyID, 16);
        AP4_ParseHex(keyCTR.c_str(), decryptionKey, 16);
        
        AP4_ProtectionKeyMap keyMap;
        keyMap.SetKeyForKid(keyID, decryptionKey, 16);

        AP4_MemoryByteStream* inputBuffer = new AP4_MemoryByteStream(reinterpret_cast<uint8_t*>(inputdata), inputbytes);
        AP4_MemoryByteStream* output = new AP4_MemoryByteStream();
        AP4_CencDecryptingProcessor* processor = new AP4_CencDecryptingProcessor(&keyMap);

        const AP4_Result result = processor->Process(*inputBuffer, *output);
        delete processor;
        inputBuffer->Release();

        if (AP4_FAILED(result))
        {
            output->Release();
            return 0;
        }

        delete[] inputdata;
        inputbytes = 0;
        
        // Fragment.
        const char*  track_selector                = NULL;
        unsigned int fragment_duration             = 0;
        bool         auto_detect_fragment_duration = true;
        bool         create_segment_index          = false;
        bool         copy_udta                     = false;
        bool         trun_version_one              = true;
        AP4_UI32     timescale                     = 0;

        AP4_MemoryByteStream* output_stream = new AP4_MemoryByteStream();
        AP4_File input_file(*output, true);

        if (input_file.GetMovie() == NULL) {
            output->Release();
            output_stream->Release();
            return inputbytes;
        }

        AP4_Array<TrackCursor*> cursors;
        TrackCursor*  video_track           = NULL;
        TrackCursor*  audio_track           = NULL;
        TrackCursor*  subtitles_track       = NULL;
        TrackCursor*  selected_track        = NULL;
        unsigned int  video_track_count     = 0;
        unsigned int  audio_track_count     = 0;
        unsigned int  subtitles_track_count = 0;

        for (AP4_List<AP4_Track>::Item* track_item = input_file.GetMovie()->GetTracks().FirstItem();
                                    track_item;
                                    track_item = track_item->GetNext()) {
            AP4_Track* track = track_item->GetData();
    
            // sanity check
            if (track->GetSampleCount() == 0 && !input_file.GetMovie()->HasFragments()) {
                continue;
            }
    
            // create a sample array for this track
            SampleArray* sample_array;
            if (input_file.GetMovie()->HasFragments()) {
                sample_array = new CachedSampleArray(track);
            } else {
                sample_array = new SampleArray(track);
            }
    
            // create a cursor for the track
            TrackCursor* cursor = new TrackCursor(track, sample_array);
            cursor->m_Tfra->SetTrackId(track->GetId());
            cursors.Append(cursor);
    
            if (track->GetType() == AP4_Track::TYPE_VIDEO) {
                if (video_track) {
                    // Warning: More than one video track found.
                } else {
                    video_track = cursor;
                }
                video_track_count++;
            } else if (track->GetType() == AP4_Track::TYPE_AUDIO) {
                if (audio_track == NULL) {
                    audio_track = cursor;
                }
                audio_track_count++;
            } else if (track->GetType() == AP4_Track::TYPE_SUBTITLES) {
                if (subtitles_track == NULL) {
                    subtitles_track = cursor;
                }
                subtitles_track_count++;
            }
        }

        if (cursors.ItemCount() == 0) {
            output->Release();
            output_stream->Release();
            return inputbytes;
        }

        if (track_selector) {
            if (!strncmp("audio", track_selector, 5)) {
                if (audio_track) {
                    selected_track = audio_track;
                } else {
                    output->Release();
                    output_stream->Release();
                    return inputbytes;
                }
            } else if (!strncmp("video", track_selector, 5)) {
                if (video_track) {
                    selected_track = video_track;
                } else {
                    output->Release();
                    output_stream->Release();
                    return inputbytes;
                }
            } else if (!strncmp("subtitles", track_selector, 9)) {
                if (subtitles_track) {
                    selected_track = subtitles_track;
                } else {
                    output->Release();
                    output_stream->Release();
                    return inputbytes;
                }
            } else {
                AP4_UI32 selected_track_id = (AP4_UI32)strtol(track_selector, NULL, 10);
                for (unsigned int i=0; i<cursors.ItemCount(); i++) {
                    if (cursors[i]->m_Track->GetId() == selected_track_id) {
                        selected_track = cursors[i];
                        break;
                    }
                }
                if (!selected_track) {
                    output->Release();
                    output_stream->Release();
                    return inputbytes;
                }
            }
        }

        if (video_track_count == 0 && audio_track_count == 0 && subtitles_track_count == 0) {
            output->Release();
            output_stream->Release();
            return inputbytes;
        }

        AP4_AvcSampleDescription* avc_desc = NULL;
        if (video_track && (Options.force_i_frame_sync != AP4_FRAGMENTER_FORCE_SYNC_MODE_NONE)) {
            // that feature is only supported for AVC
            AP4_SampleDescription* sdesc = video_track->m_Track->GetSampleDescription(0);
            if (sdesc) {
                avc_desc = AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, sdesc);
            }
            if (avc_desc == NULL) {
                output->Release();
                output_stream->Release();
                return inputbytes;
            }
        }

        AP4_Position position;
        output->Tell(position);

        if (input_file.GetMovie()->HasFragments()) {
            AP4_LinearReader reader(*input_file.GetMovie(), output);
            for (unsigned int i=0; i<cursors.ItemCount(); i++) {
                reader.EnableTrack(cursors[i]->m_Track->GetId());
            }
            AP4_UI32 track_id;
            AP4_Sample sample;
            do {
                result = reader.GetNextSample(sample, track_id);
                if (AP4_SUCCEEDED(result)) {
                    for (unsigned int i=0; i<cursors.ItemCount(); i++) {
                        if (cursors[i]->m_Track->GetId() == track_id) {
                            cursors[i]->m_Samples->AddSample(sample);
                            break;
                        }
                    }
                }
            } while (AP4_SUCCEEDED(result));
            
        } else if (video_track && (Options.force_i_frame_sync != AP4_FRAGMENTER_FORCE_SYNC_MODE_NONE)) {
            AP4_Sample sample;
            if (Options.force_i_frame_sync == AP4_FRAGMENTER_FORCE_SYNC_MODE_AUTO) {
                // detect if this looks like an open-gop source
                for (unsigned int i=1; i<video_track->m_Samples->GetSampleCount(); i++) {
                    if (AP4_SUCCEEDED(video_track->m_Samples->GetSample(i, sample))) {
                        if (sample.IsSync()) {
                            // we found a sync i-frame, assume this is *not* an open-gop source
                            Options.force_i_frame_sync = AP4_FRAGMENTER_FORCE_SYNC_MODE_NONE;
                            break;
                        }
                    }
                }
            }
            if (Options.force_i_frame_sync != AP4_FRAGMENTER_FORCE_SYNC_MODE_NONE) {
                for (unsigned int i=0; i<video_track->m_Samples->GetSampleCount(); i++) {
                    if (AP4_SUCCEEDED(video_track->m_Samples->GetSample(i, sample))) {
                        if (IsIFrame(sample, avc_desc)) {
                            video_track->m_Samples->ForceSync(i);
                        }
                    }
                }
            }
        }

        output->Seek(position);

        if (auto_detect_fragment_duration) {
            if (video_track) {
                fragment_duration = AutoDetectFragmentDuration(video_track);
            } else if (audio_track && input_file.GetMovie()->HasFragments()) {
                fragment_duration = AutoDetectAudioFragmentDuration(*output, audio_track);
            }
            if (fragment_duration == 0) {
                fragment_duration = AP4_FRAGMENTER_DEFAULT_FRAGMENT_DURATION;
            } else if (fragment_duration > AP4_FRAGMENTER_MAX_AUTO_FRAGMENT_DURATION) {
                fragment_duration = AP4_FRAGMENTER_DEFAULT_FRAGMENT_DURATION;
            }
        }

        AP4_Array<TrackCursor*> tracks_to_fragment;
        if (selected_track) {
            tracks_to_fragment.Append(selected_track);
        } else {
            tracks_to_fragment = cursors;
        }
        Fragment(input_file, *output_stream, tracks_to_fragment, fragment_duration, timescale, create_segment_index, copy_udta, trun_version_one);

        inputbytes = output_stream->GetDataSize();
        inputdata = new uint8_t[inputbytes];
        memcpy(inputdata, output_stream->GetData(), inputbytes);

        if (output)  output->Release();
        if (output_stream) output_stream->Release();
    }
    else if(encryption.method != CommonEncryption::Method::None)
    {
        inputbytes = 0;
    }

    return inputbytes;
}
