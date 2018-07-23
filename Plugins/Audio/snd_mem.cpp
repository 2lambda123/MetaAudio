#include <metahook.h>
#include <optional>

#include "FileSystem.h"
#include "util.h"
#include "exportfuncs.h"
#include "snd_local.h"
#include "zone.h"

static auto local_decoder = alure::MakeShared<LocalAudioDecoder>();

// Check if file exists. Order: .wav, .flac, .ogg
static std::optional<alure::String> S_GetFilePath(alure::String sfx_name, bool is_stream)
{
  FileHandle_t hFile = nullptr;
  alure::String namebuffer;

  if (is_stream)
  {
    sfx_name.erase(sfx_name.begin());
  }

  // Search for "." from right to left
  int char_index = sfx_name.size() - 1;
  while (sfx_name[char_index] != '.')
  {
    char_index--;
    if (char_index < 0)
    {
      break;
    }
  }

  if (char_index > 0)
  {
    for (const alure::String &extension : LocalAudioDecoder::SupportedExtensions)
    {
      sfx_name.replace(char_index, sfx_name.npos, extension);

      namebuffer = "sound";
      if (sfx_name[0] != '/')
      {
        namebuffer.append("/");
      }

      namebuffer.append(sfx_name);

      hFile = g_pFileSystem->Open(namebuffer.c_str(), "rb");

      if (!hFile)
      {
        namebuffer.clear();
        if (sfx_name[0] != '/')
        {
          namebuffer.append("/");
        }
        namebuffer.append(sfx_name);

        hFile = g_pFileSystem->Open(namebuffer.c_str(), "rb");
      }

      if (hFile)
      {
        break;
      }
    }
  }

  if (!hFile)
  {
    if (is_stream)
    {
      gEngfuncs.Con_DPrintf("S_LoadStreamSound: Couldn't load %s.\n", namebuffer.c_str());
    }
    else
    {
      gEngfuncs.Con_DPrintf("S_LoadSound: Couldn't load %s.\n", namebuffer.c_str());
    }
    
    return std::optional<alure::String>{};
  }
  else
  {
    g_pFileSystem->Close(hFile);
    hFile = nullptr;

    char al_file_path[MAX_PATH];
    g_pFileSystem->GetLocalPath(namebuffer.c_str(), al_file_path, sizeof(al_file_path));
    return alure::String(al_file_path);
  }
}

aud_sfxcache_t *S_LoadStreamSound(sfx_t *s, aud_channel_t *ch)
{
  // Some VOX_ that should be stream may not be set as streaming. Fix it here.
  ch->entchannel = CHAN_STREAM;

  std::vector<byte> data;
  aud_sfxcache_t *sc;

  qboolean ffileopened = false;

  //if (cl.fPrecaching)
  //	return nullptr;

  if (ch == nullptr)
    return nullptr;

  sc = (aud_sfxcache_t *)Cache_Check(&s->cache);
  if (sc && sc->decoder)
  {
    ffileopened = true;
  }

  //Alloc cache if we don't have one
  if (sc == nullptr)
  {
    sc = (aud_sfxcache_t *)Cache_Alloc(&s->cache, sizeof(aud_sfxcache_t), s->name);
    if (sc == nullptr)
      return nullptr;

    //Clear before use
    memset(sc, 0, sizeof(aud_sfxcache_t));
  }

  std::optional<alure::String> file_path;
  if (!ffileopened)
  {
    file_path = S_GetFilePath(s->name, true);
    if (!file_path.has_value())
    {
      return nullptr;
    }
  }

  // For OpenAL
  if (sc->decoder == nullptr && file_path.has_value())
  {
    auto context = alure::Context::GetCurrent();
    try
    {
      sc->decoder = context.createDecoder(file_path.value());
      sc->length = sc->decoder->getLength();
      sc->samplerate = sc->decoder->getFrequency();
      sc->stype = sc->decoder->getSampleType();
      sc->channels = sc->decoder->getChannelConfig();

      if (sc->decoder->hasLoopPoints())
      {
        auto loop_points = sc->decoder->getLoopPoints();
        sc->loopstart = loop_points.first;
        sc->loopend = loop_points.second;
      }
      else
      {
        sc->loopstart = -1;
        sc->loopend = INT_MAX;
      }
    }
    catch (const std::exception& error)
    {
      gEngfuncs.Con_DPrintf("S_LoadStreamSound: %s: %s\n", file_path.value(), error.what());
      return nullptr;
    }
  }

  return sc;
}

aud_sfxcache_t *S_LoadSound(sfx_t *s, aud_channel_t *ch)
{
  if (!openal_enabled)
  {
    return gAudEngine.S_LoadSound(s, ch);
  }

  aud_sfxcache_t *sc;

  if (s->name[0] == '*')
    return S_LoadStreamSound(s, ch);

  if (s->name[0] == '?')
  {
    try
    {
      alure::SharedPtr<VoiceDecoder> dec = alure::MakeShared<VoiceDecoder>(VoiceDecoder(s, ch));
      sc = new aud_sfxcache_t();
      sc->channels = dec->getChannelConfig();
      sc->samplerate = dec->getFrequency();
      sc->length = INT_MAX;
      sc->loopstart = -1;
      sc->loopend = INT_MAX;
      sc->stype = dec->getSampleType();
      sc->decoder = std::static_pointer_cast<alure::Decoder>(dec);
      return sc;
    }
    catch (const std::runtime_error& error)
    {
      gEngfuncs.Con_DPrintf("S_LoadSound: Unable to start voice playback. %s.\n", error.what());
      return nullptr;
    }
  }

  sc = (aud_sfxcache_t *)Cache_Check(&s->cache);
  if (sc)
    return sc;

  auto context = alure::Context::GetCurrent();
  if (local_decoder != context.getMessageHandler())
  {
    context.setMessageHandler(local_decoder);
  }

  std::optional<alure::String> file_path = S_GetFilePath(s->name, false);
  if (!file_path.has_value())
  {
    return nullptr;
  }

  alure::Buffer al_buffer;
  try
  {
    al_buffer = context.getBuffer(file_path.value());
  }
  catch (const std::exception& error)
  {
    gEngfuncs.Con_DPrintf("S_LoadSound: %s: %s\n", file_path.value().c_str(), error.what());
    sc = nullptr;
    return nullptr;
  }

  sc = (aud_sfxcache_t *)Cache_Alloc(&s->cache, sizeof(aud_sfxcache_t), s->name);
  if (sc == nullptr)
    return nullptr;

  memset(sc, 0, sizeof(aud_sfxcache_t));

  wavinfo_t info = wavinfo_t();
  //We can't interfere with Alure, so we need a copy of the data for mouth movement.
  if (!local_decoder->GetWavinfo(info, file_path.value(), sc->data))
    return nullptr;

  sc->buffer = alure::MakeShared<alure::Buffer>(al_buffer);
  sc->length = info.samples; //number of samples ( include channels )
  sc->loopstart = info.loopstart; //-1 or loop start position
  sc->loopend = info.loopend;
  sc->samplerate = info.samplerate;
  sc->stype = info.stype;
  sc->channels = info.channels;

  // Set loop points if needed
  if (sc->loopstart > 0)
  {
    try
    {
      auto points = sc->buffer->getLoopPoints();
      if (points.first != sc->loopstart)
      {
        sc->buffer->setLoopPoints(sc->loopstart, sc->loopend ? sc->loopend : sc->buffer->getLength());
      }
    }
    catch (const std::exception& error)
    {
      gEngfuncs.Con_DPrintf("Unable to set loop points for sound %s. %s. Will use manual looping.\n", s->name, error.what());
    }
  }

  return sc;
}