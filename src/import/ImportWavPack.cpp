/**********************************************************************
  Audacity: A Digital Audio Editor

  Audacity(R) is copyright (c) 1999-2021 Audacity Team.

  License: GPL-v2-or-later.  See License.txt.

  ImportWavPack.cpp

  Subhradeep Chakraborty

  Edited by:

  Zachary Gerbi
*//****************************************************************//**
\class WavPackImportFileHandle
\brief An ImportFileHandle for WavPack data
*//****************************************************************//**
\class WavPackImportPlugin
\brief An ImportPlugin for WavPack data
*//*******************************************************************/

#pragma once

#include "Import.h"
#include "ImportPlugin.h"

#include<wx/string.h>
#include<stdlib.h>

#include "Prefs.h"
#include "../Tags.h"
#include "../WaveTrack.h"
#include "../widgets/ProgressDialog.h"

#define DESC XO("WavPack files")

static const auto exts = {
   wxT("wv")
};

#ifndef USE_WAVPACK

static Importer::RegisteredUnusableImportPlugin registered
{
   std::make_unique<UnusableImportPlugin>(DESC, FileExtensions(exts.begin(), exts.end()))
};

#else

extern "C" {
#include<wavpack.h>
}


class WavPackImportPlugin final : public ImportPlugin
{
public:
   WavPackImportPlugin();
   ~WavPackImportPlugin();

   wxString GetPluginStringID() override;
   TranslatableString GetPluginFormatDescription() override;
   std::unique_ptr<ImportFileHandle> Open(
      const FilePath& Filename, AudacityProject*) override;
};

using NewChannelGroup = std::vector< std::shared_ptr<WaveTrack> >;

class WavPackImportFileHandle final : public ImportFileHandle
{
public:
   WavPackImportFileHandle(const FilePath& filename, WavpackContext* wavpackContext);
   ~WavPackImportFileHandle();

   TranslatableString GetFileDescription() override;
   ByteCount GetFileUncompressedBytes() override;
   ProgressResult Import(WaveTrackFactory* trackFactory, TrackHolders& outTracks, Tags* tags) override;
   wxInt32 GetStreamCount() override;
   const TranslatableStrings& GetStreamInfo() override;
   void SetStreamUsage(wxInt32 StreamID, bool Use) override;

private:
   WavpackContext* mWavPackContext;
   int mNumChannels;
   uint32_t mSampleRate;
   int mBitsPerSample;
   int mBytesPerSample;
   int64_t mNumSamples;
   ProgressResult mUpdateResult;
   NewChannelGroup mChannels;
   sampleFormat mFormat;
};

// ============================================================================
// WavPackImportPlugin
// ============================================================================

WavPackImportPlugin::WavPackImportPlugin()
   : ImportPlugin(FileExtensions(exts.begin(), exts.end()))
{
}

WavPackImportPlugin::~WavPackImportPlugin()
{
}

wxString WavPackImportPlugin::GetPluginStringID()
{
   return wxT("libwavpack");
}

TranslatableString WavPackImportPlugin::GetPluginFormatDescription()
{
   return DESC;
}

std::unique_ptr<ImportFileHandle> WavPackImportPlugin::Open(const FilePath& filename, AudacityProject*)
{
   char errMessage[100]; // To hold possible error message
   int flags = OPEN_WVC | OPEN_FILE_UTF8 | OPEN_TAGS;
   WavpackContext* wavpackContext = WavpackOpenFileInput(filename, errMessage, flags, 0);

   if (!wavpackContext) {
      // Some error occured(e.g. File not found or is invalid)
      return nullptr;
   }

   auto handle = std::make_unique<WavPackImportFileHandle>(filename, wavpackContext);

   return std::move(handle);
}

static Importer::RegisteredImportPlugin registered{ "WavPack",
   std::make_unique< WavPackImportPlugin >()
};

// ============================================================================
// WavPackImportFileHandle
// ============================================================================

WavPackImportFileHandle::WavPackImportFileHandle(const FilePath& filename,
   WavpackContext* wavpackContext)
   : ImportFileHandle(filename),
   mWavPackContext(wavpackContext)
{
   mNumChannels = WavpackGetNumChannels(mWavPackContext);
   mSampleRate = WavpackGetSampleRate(mWavPackContext);
   mBitsPerSample = WavpackGetBitsPerSample(mWavPackContext);
   mBytesPerSample = WavpackGetBytesPerSample(mWavPackContext);
   mNumSamples = WavpackGetNumSamples64(mWavPackContext);

   if (mBitsPerSample <= 16) {
      mFormat = int16Sample;
   }
   else if (mBitsPerSample <= 24) {
      mFormat = int24Sample;
   }
   else {
      mFormat = floatSample;
   }
}

TranslatableString WavPackImportFileHandle::GetFileDescription()
{
   return DESC;
}

auto WavPackImportFileHandle::GetFileUncompressedBytes() -> ByteCount
{
   return 0;
}

ProgressResult WavPackImportFileHandle::Import(WaveTrackFactory* trackFactory, TrackHolders& outTracks, Tags* tags)
{
   outTracks.clear();

   CreateProgress();

   mChannels.resize(mNumChannels);

   {
      auto iter = mChannels.begin();
      for (size_t c = 0; c < mNumChannels; ++iter, ++c)
         *iter = NewWaveTrack(*trackFactory, mFormat, mSampleRate);
   }

   /* The number of samples to read in each loop */
#define SAMPLES_TO_READ 100000
   auto updateResult = ProgressResult::Success;
   uint32_t totalSamplesRead = 0;

   {
      uint32_t bufferSize = mNumChannels * SAMPLES_TO_READ;
      ArrayOf<int32_t> wavpackBuffer{ bufferSize };
      uint32_t samplesRead = 0;

      do {
         samplesRead = WavpackUnpackSamples(mWavPackContext, wavpackBuffer.get(), SAMPLES_TO_READ);

         for (int64_t c = 0; c < samplesRead * mNumChannels;) {
            auto iter = mChannels.begin();
            for (unsigned chn = 0; chn < mNumChannels; ++iter, ++c, ++chn) {
               iter->get()->Append((char*)&wavpackBuffer[c], mFormat, 1);
            }
         }

         totalSamplesRead += samplesRead;
         updateResult = mProgress->Update(WavpackGetProgress(mWavPackContext), 1.0);
      } while (updateResult == ProgressResult::Success && samplesRead != 0);
   }

   if (updateResult != ProgressResult::Stopped && totalSamplesRead < mNumSamples)
      updateResult = ProgressResult::Failed;

   if (updateResult == ProgressResult::Failed || updateResult == ProgressResult::Cancelled)
      return updateResult;

   for (const auto& channel : mChannels)
      channel->Flush();

   if (!mChannels.empty())
      outTracks.push_back(std::move(mChannels));

   int wavpackMode = WavpackGetMode(mWavPackContext);
   if (wavpackMode & MODE_VALID_TAG) {
      bool apeTag = wavpackMode & MODE_APETAG;
      int numItems = WavpackGetNumTagItems(mWavPackContext);

      if (numItems > 0) {
         tags->Clear();
         for (int i = 0; i < numItems; i++) {
            int itemLen = 0, valueLen = 0;
            char* item, * itemValue;
            wxString value, name;

            // Get the actual length of the item key at this index i
            itemLen = WavpackGetTagItemIndexed(mWavPackContext, i, NULL, 0);
            item = (char*)malloc(itemLen + 1);
            WavpackGetTagItemIndexed(mWavPackContext, i, item, itemLen + 1);
            name = UTF8CTOWX(item);

            // Get the actual length of the value for this item key
            valueLen = WavpackGetTagItem(mWavPackContext, item, NULL, 0);
            itemValue = (char*)malloc(valueLen + 1);
            WavpackGetTagItem(mWavPackContext, item, itemValue, valueLen + 1);

            if (apeTag) {
               for (int j = 0; j < valueLen; j++) {
                  // APEv2 text tags can have multiple NULL separated string values
                  if (!itemValue[j]) {
                     itemValue[j] = '\\';
                  }
               }
            }
            value = UTF8CTOWX(itemValue);

            if (name.Upper() == wxT("DATE") && !tags->HasTag(TAG_YEAR)) {
               long val;
               if (value.length() == 4 && value.ToLong(&val)) {
                  name = TAG_YEAR;
               }
            }

            tags->SetTag(name, value);

            free(item);
            free(itemValue);
         }
      }
   }

   return updateResult;
}

wxInt32 WavPackImportFileHandle::GetStreamCount()
{
   return 1;
}

const TranslatableStrings& WavPackImportFileHandle::GetStreamInfo()
{
   static TranslatableStrings empty;
   return empty;
}

void WavPackImportFileHandle::SetStreamUsage(wxInt32 WXUNUSED(StreamID), bool WXUNUSED(Use))
{
}

WavPackImportFileHandle::~WavPackImportFileHandle()
{
   WavpackCloseFile(mWavPackContext);
}

#endif 
