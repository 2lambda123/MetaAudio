#include <metahook.h>
#include "exportfuncs.h"
#include "FileSystem.h"
#include "util.h"
#include "snd_local.h"
#include <math.h>
#include "alure/AL/efx-presets.h"

extern cvar_t *sxroom_off;
extern cvar_t *sxroomwater_type;
extern cvar_t *sxroom_type;
static cvar_t *al_occlusion = nullptr;

static constexpr int PM_NORMAL = 0x00000000;
static constexpr int  PM_STUDIO_IGNORE = 0x00000001;     // Skip studio models
static constexpr int  PM_STUDIO_BOX = 0x00000002;        // Use boxes for non-complex studio models (even in traceline)
static constexpr int  PM_GLASS_IGNORE = 0x00000004;      // Ignore entities with non-normal rendermode
static constexpr int  PM_WORLD_ONLY = 0x00000008;        // Only trace against the world

static alure::Effect alReverbEffects[CSXROOM];
static alure::AuxiliaryEffectSlot alAuxEffectSlots;

// HL1 DSPROPERTY_EAXBUFFER_REVERBMIX seems to be always set to 0.38,
// with no adjustment of reverb intensity with distance.
// Reverb adjustment with distance is disabled per-source.
static constexpr float AL_REVERBMIX = 0.38f;
static constexpr float AL_MAX_DISTANCE_INCHES = 1000.0f;
static constexpr float AL_SND_GAIN_FADE_TIME = 0.25f;

static constexpr float AL_UNDERWATER_LP_GAIN = 0.25f;

// Creative X-Fi's are buggy with the direct filter gain set to 1.0f,
// they get stuck.
static constexpr float gain_epsilon = 1.0f - std::numeric_limits<float>::epsilon();

static EFXEAXREVERBPROPERTIES presets_room[CSXROOM] = {
    EFX_REVERB_PRESET_GENERIC,                    //  0
  //SXROOM_GENERIC
    EFX_REVERB_PRESET_ROOM,                       //  1
  //SXROOM_METALIC_S
    EFX_REVERB_PRESET_BATHROOM,                   //  2
    EFX_REVERB_PRESET_BATHROOM,                   //  3
    EFX_REVERB_PRESET_BATHROOM,                   //  4
  //SXROOM_TUNNEL_S
    EFX_REVERB_PRESET_SEWERPIPE,                  //  4
    EFX_REVERB_PRESET_SEWERPIPE,                  //  6
    EFX_REVERB_PRESET_SEWERPIPE,                  //  7
  //SXROOM_CHAMBER_S
    EFX_REVERB_PRESET_STONEROOM,                  //  8
    EFX_REVERB_PRESET_STONEROOM,                  //  9
    EFX_REVERB_PRESET_STONEROOM,                  // 10
  //SXROOM_BRITE_S
    EFX_REVERB_PRESET_STONECORRIDOR,              // 11
    EFX_REVERB_PRESET_STONECORRIDOR,              // 12
    EFX_REVERB_PRESET_STONECORRIDOR,              // 13
  //SXROOM_WATER1
    EFX_REVERB_PRESET_UNDERWATER,                 // 14
    EFX_REVERB_PRESET_UNDERWATER,                 // 15
    EFX_REVERB_PRESET_UNDERWATER,                 // 16
  //SXROOM_CONCRETE_S
    EFX_REVERB_PRESET_GENERIC,                    // 17
    EFX_REVERB_PRESET_GENERIC,                    // 18
    EFX_REVERB_PRESET_GENERIC,                    // 19
  //SXROOM_OUTSIDE1
    EFX_REVERB_PRESET_ARENA,                      // 20
    EFX_REVERB_PRESET_ARENA,                      // 21
    EFX_REVERB_PRESET_ARENA,                      // 22
  //SXROOM_CAVERN_S
    EFX_REVERB_PRESET_CONCERTHALL,                // 23
    EFX_REVERB_PRESET_CONCERTHALL,                // 24
    EFX_REVERB_PRESET_CONCERTHALL,                // 25
  //SXROOM_WEIRDO1
    EFX_REVERB_PRESET_DIZZY,                      // 26
    EFX_REVERB_PRESET_DIZZY,                      // 27
    EFX_REVERB_PRESET_DIZZY                       // 28
};

struct pmplane_t
{
  vec3_t	normal;
  float	dist;
};

struct pmtrace_s
{
  qboolean	allsolid;		       // if true, plane is not valid
  qboolean	startsolid;	       // if true, the initial point was in a solid area
  qboolean	inopen, inwater;   // End point is in empty space or in water
  float	fraction;              // time completed, 1.0 = didn't hit anything
  vec3_t	endpos;              // final position
  pmplane_t	plane;             // surface normal at impact
  int	ent;                     // entity at impact
  vec3_t	deltavelocity;       // Change in player's velocity caused by impact.  
                               // Only run on server.
  int	hitgroup;
};

pmtrace_s *CL_TraceLine(vec3_t start, vec3_t end, int flags)
{
  pmtrace_s *tr = gEngfuncs.PM_TraceLine(start, end, flags, 2, -1);
  return tr;
}

float SND_FadeToNewGain(aud_channel_t *ch, float gain_new)
{
  float	speed, frametime;
  frametime = (*gAudEngine.cl_time) - (*gAudEngine.cl_oldtime);
  if (frametime == 0.0f)
  {
    return ch->ob_gain;
  }

  if (gain_new == -1.0)
  {
    // if -1 passed in, just keep fading to existing target
    gain_new = ch->ob_gain_target;
  }

  // if first time updating, store new gain into gain & target, return
  // if gain_new is close to existing gain, store new gain into gain & target, return
  if (ch->firstpass || (fabs(gain_new - ch->ob_gain) < 0.01f))
  {
    ch->ob_gain = gain_new;
    ch->ob_gain_target = gain_new;
    ch->ob_gain_inc = 0.0f;
    return gain_new;
  }

  // set up new increment to new target
  speed = (frametime / AL_SND_GAIN_FADE_TIME) * (gain_new - ch->ob_gain);

  ch->ob_gain_inc = fabs(speed);

  // ch->ob_gain_inc = fabs( gain_new - ch->ob_gain ) / 10.0f;
  ch->ob_gain_target = gain_new;

  // if not hit target, keep approaching
  if (fabs(ch->ob_gain - ch->ob_gain_target) > 0.01f)
  {
    ch->ob_gain = ApproachVal(ch->ob_gain_target, ch->ob_gain, ch->ob_gain_inc);
  }
  else
  {
    // close enough, set gain = target
    ch->ob_gain = ch->ob_gain_target;
  }

  return ch->ob_gain;
}

float SND_FadeToNewSendGain(aud_channel_t *ch, float send_gain_new)
{
  float	speed, frametime;
  frametime = (*gAudEngine.cl_time) - (*gAudEngine.cl_oldtime);
  if (frametime == 0.0f)
  {
    return ch->ob_send_gain;
  }

  if (send_gain_new == -1.0)
  {
    // if -1 passed in, just keep fading to existing target
    send_gain_new = ch->ob_send_gain_target;
  }

  // if first time updating, store new gain into gain & target, return
  // if gain_new is close to existing gain, store new gain into gain & target, return
  if (ch->firstpass_send || (fabs(send_gain_new - ch->ob_send_gain) < 0.01f))
  {
    ch->ob_send_gain = send_gain_new;
    ch->ob_send_gain_target = send_gain_new;
    ch->ob_send_gain_inc = 0.0f;
    return send_gain_new;
  }

  // set up new increment to new target
  speed = (frametime / AL_SND_GAIN_FADE_TIME) * (send_gain_new - ch->ob_send_gain);

  ch->ob_send_gain_inc = fabs(speed);

  // ch->ob_send_gain_inc = fabs( gain_new - ch->ob_send_gain ) / 10.0f;
  ch->ob_send_gain_target = send_gain_new;

  // if not hit target, keep approaching
  if (fabs(ch->ob_send_gain - ch->ob_send_gain_target) > 0.01f)
  {
    ch->ob_send_gain = ApproachVal(ch->ob_send_gain_target, ch->ob_send_gain, ch->ob_send_gain_inc);
  }
  else
  {
    // close enough, set gain = target
    ch->ob_send_gain = ch->ob_send_gain_target;
  }

  return ch->ob_send_gain;
}

float SX_GetGainObscured(aud_channel_t *ch, cl_entity_t *pent, cl_entity_t *sent)
{
  float	gain = gain_epsilon;
  vec3_t	endpoint;
  int	count = 1;
  pmtrace_s	*tr;

  // set up traceline from player eyes to sound emitting entity origin
  VectorCopy(ch->origin, endpoint);

  tr = CL_TraceLine(pent->origin, endpoint, PM_STUDIO_IGNORE);

  if ((tr->fraction < 1.0f || tr->allsolid || tr->startsolid) && tr->fraction < 0.99f)
  {
    // can't see center of sound source:
    // build extents based on dB sndlvl of source,
    // test to see how many extents are visible,
    // drop gain by g_snd_obscured_loss_db per extent hidden
    vec3_t	endpoints[4];
    int	i;
    vec3_t	vecl, vecr, vecl2, vecr2;
    vec3_t	vsrc_forward;
    vec3_t	vsrc_right;
    vec3_t	vsrc_up;
    float	radius = 0;

    // get radius
    if (sent->model != nullptr)
    {
      radius = sent->model->radius;
    }      
    else
    {
      if (ch->attenuation)
      {
        radius = (20 * log10(pow(10, 3) / (ch->attenuation * 36))); // sndlvl
        radius = 24 + (240 - 24) * (radius - 60) / (140 - 60); // radius
      }
    }      

    // set up extent endpoints - on upward or downward diagonals, facing player
    for (i = 0; i < 4; i++)
      VectorCopy(endpoint, endpoints[i]);

    // vsrc_forward is normalized vector from sound source to listener
    VectorSubtract(pent->origin, endpoint, vsrc_forward);
    VectorNormalize(vsrc_forward);
    VectorVectors(vsrc_forward, vsrc_right, vsrc_up);

    VectorAdd(vsrc_up, vsrc_right, vecl);

    // if src above listener, force 'up' vector to point down - create diagonals up & down
    if (endpoint[2] > pent->origin[2] + (10 * 12))
      vsrc_up[2] = -vsrc_up[2];

    VectorSubtract(vsrc_up, vsrc_right, vecr);
    VectorNormalize(vecl);
    VectorNormalize(vecr);

    // get diagonal vectors from sound source 
    VectorScale(vecl, radius, vecl2);
    VectorScale(vecr, radius, vecr2);
    VectorScale(vecl, (radius / 2.0f), vecl);
    VectorScale(vecr, (radius / 2.0f), vecr);

    // endpoints from diagonal vectors
    VectorAdd(endpoints[0], vecl, endpoints[0]);
    VectorAdd(endpoints[1], vecr, endpoints[1]);
    VectorAdd(endpoints[2], vecl2, endpoints[2]);
    VectorAdd(endpoints[3], vecr2, endpoints[3]);

    // drop gain for each point on radius diagonal that is obscured
    for (count = 0, i = 0; i < 4; i++)
    {
      // UNDONE: some endpoints are in walls - in this case, trace from the wall hit location
      tr = CL_TraceLine(pent->origin, endpoints[i], PM_STUDIO_IGNORE);

      if ((tr->fraction < 1.0f || tr->allsolid || tr->startsolid) && tr->fraction < 0.99f && !tr->startsolid)
      {
        // skip first obscured point: at least 2 points + center should be obscured to hear db loss
        if (++count > 1)
          gain = gain * pow(10, -2.70f / 20.0f);
      }
    }
  }

  gain = SND_FadeToNewGain(ch, gain);

  return gain;
}

void SX_ApplyEffect(aud_channel_t *ch, int roomtype, qboolean underwater)
{
  float direct_gain = gain_epsilon;
  float send_gain = gain_epsilon;
  cl_entity_t *pent = gEngfuncs.GetEntityByIndex(*gAudEngine.cl_viewentity);
  cl_entity_t *sent = gEngfuncs.GetEntityByIndex(ch->entnum);
  if (ch->entnum != *gAudEngine.cl_viewentity && pent != nullptr && sent != nullptr)
  {
    // Detect collisions and reduce gain on occlusion
    if (al_occlusion->value)
    {
      direct_gain = SX_GetGainObscured(ch, pent, sent);
    }

    // Manually do reverb gain adjustment as the distance model only affects
    // the source gain.
    auto distance = alure::Vector3(ch->origin[0], ch->origin[1], ch->origin[2]).getDistance(
      alure::Vector3(pent->origin[0], pent->origin[1], pent->origin[2]));
    send_gain = max(min(1.0f - distance * ch->attenuation / AL_MAX_DISTANCE_INCHES, gain_epsilon), 0.0f);
    send_gain = SND_FadeToNewSendGain(ch, send_gain);
  }

  if (roomtype > 0 && roomtype < CSXROOM && sxroom_off && !sxroom_off->value)
  {
    if (underwater)
    {
      alAuxEffectSlots.applyEffect(alReverbEffects[14]);
      ch->source.setDirectFilter(alure::FilterParams{ direct_gain, AL_UNDERWATER_LP_GAIN, AL_HIGHPASS_DEFAULT_GAIN });
    }
    else
    {
      alAuxEffectSlots.applyEffect(alReverbEffects[roomtype]);
      ch->source.setDirectFilter(alure::FilterParams{ direct_gain, AL_LOWPASS_DEFAULT_GAIN, AL_HIGHPASS_DEFAULT_GAIN });
    }
    ch->source.setAuxiliarySendFilter(alAuxEffectSlots, 0, alure::FilterParams{ send_gain, AL_LOWPASS_DEFAULT_GAIN, AL_HIGHPASS_DEFAULT_GAIN });
  }
  else
  {
    alAuxEffectSlots.applyEffect(alReverbEffects[0]);
    ch->source.setAuxiliarySendFilter(alAuxEffectSlots, 0, alure::FilterParams{ send_gain, AL_LOWPASS_DEFAULT_GAIN, AL_HIGHPASS_DEFAULT_GAIN });

    if (underwater)
      ch->source.setDirectFilter(alure::FilterParams{ direct_gain, AL_UNDERWATER_LP_GAIN, AL_HIGHPASS_DEFAULT_GAIN });
    else
      ch->source.setDirectFilter(alure::FilterParams{ direct_gain, AL_LOWPASS_DEFAULT_GAIN, AL_HIGHPASS_DEFAULT_GAIN });
  }
}

void SX_Init(void)
{
  alure::Context al_context = alure::Context::GetCurrent();
  al_occlusion = gEngfuncs.pfnRegisterVariable("al_occlusion", "1", 0);

  // Disable reverb when room_type = 0:
  presets_room[0].flGain = 0;

  // HL uses EAX 1.0, which are really different from non-EAX reverbs.
  // flGain = EAX 1.0 volume
  // flDecayTime = EAX 1.0 decay time
  // flDecayHFRatio = EAX 1.0 damping (probably)
  presets_room[1].flGain = 0.417f;
  presets_room[1].flDecayTime = 0.4f;
  presets_room[1].flDecayHFRatio = static_cast<float>(2 / 3);

  presets_room[2].flGain = 0.3f;
  presets_room[2].flDecayTime = 1.5f;
  presets_room[2].flDecayHFRatio = static_cast<float>(1 / 6);

  presets_room[3].flGain = 0.4f;
  presets_room[3].flDecayTime = 1.5f;
  presets_room[3].flDecayHFRatio = static_cast<float>(1 / 6);

  presets_room[4].flGain = 0.6f;
  presets_room[4].flDecayTime = 1.5f;
  presets_room[4].flDecayHFRatio = static_cast<float>(1 / 6);

  presets_room[5].flGain = 0.4f;
  presets_room[5].flDecayTime = 2.886f;
  presets_room[5].flDecayHFRatio = 0.25;

  presets_room[6].flGain = 0.6f;
  presets_room[6].flDecayTime = 2.886f;
  presets_room[6].flDecayHFRatio = 0.25f;

  presets_room[7].flGain = 0.8f;
  presets_room[7].flDecayTime = 2.886f;
  presets_room[7].flDecayHFRatio = 0.25f;

  presets_room[8].flGain = 0.5f;
  presets_room[8].flDecayTime = 2.309f;
  presets_room[8].flDecayHFRatio = 0.888f;

  presets_room[9].flGain = 0.65f;
  presets_room[9].flDecayTime = 2.309f;
  presets_room[9].flDecayHFRatio = 0.888f;

  presets_room[10].flGain = 0.8f;
  presets_room[10].flDecayTime = 2.309f;
  presets_room[10].flDecayHFRatio = 0.888f;

  presets_room[11].flGain = 0.3f;
  presets_room[11].flDecayTime = 2.697f;
  presets_room[11].flDecayHFRatio = 0.638f;

  presets_room[12].flGain = 0.5f;
  presets_room[11].flDecayTime = 2.697f;
  presets_room[11].flDecayHFRatio = 0.638f;

  presets_room[13].flGain = 0.65f;
  presets_room[11].flDecayTime = 2.697f;
  presets_room[11].flDecayHFRatio = 0.638f;

  presets_room[14].flGain = 1.0f;
  presets_room[14].flDecayTime = 1.5f;
  presets_room[14].flDecayHFRatio = 0.0f;

  presets_room[15].flGain = 1.0f;
  presets_room[15].flDecayTime = 2.5f;
  presets_room[15].flDecayHFRatio = 0.0f;

  presets_room[16].flGain = 1.0f;
  presets_room[16].flDecayTime = 3.5f;
  presets_room[16].flDecayHFRatio = 0.0f;

  presets_room[17].flGain = 0.65f;
  presets_room[17].flDecayTime = 1.493f;
  presets_room[17].flDecayHFRatio = 0.5f;

  presets_room[18].flGain = 0.85f;
  presets_room[18].flDecayTime = 1.493f;
  presets_room[18].flDecayHFRatio = 0.5f;

  presets_room[19].flGain = 1.0f;
  presets_room[19].flDecayTime = 1.493f;
  presets_room[19].flDecayHFRatio = 0.5f;

  presets_room[20].flGain = 0.4f;
  presets_room[20].flDecayTime = 7.284f;
  presets_room[20].flDecayHFRatio = static_cast<float>(1 / 3);

  presets_room[21].flGain = 0.55f;
  presets_room[21].flDecayTime = 7.284f;
  presets_room[21].flDecayHFRatio = static_cast<float>(1 / 3);

  presets_room[22].flGain = 0.7f;
  presets_room[22].flDecayTime = 7.284f;
  presets_room[22].flDecayHFRatio = static_cast<float>(1 / 3);

  presets_room[23].flGain = 0.5f;
  presets_room[23].flDecayTime = 3.961f;
  presets_room[23].flDecayHFRatio = 0.5f;

  presets_room[24].flGain = 0.7f;
  presets_room[24].flDecayTime = 3.961f;
  presets_room[24].flDecayHFRatio = 0.5f;

  presets_room[25].flGain = 1.0f;
  presets_room[25].flDecayTime = 3.961f;
  presets_room[25].flDecayHFRatio = 0.5f;

  presets_room[26].flGain = 0.2f;
  presets_room[26].flDecayTime = 17.234f;
  presets_room[26].flDecayHFRatio = static_cast<float>(2 / 3);

  presets_room[27].flGain = 0.3f;
  presets_room[27].flDecayTime = 17.234f;
  presets_room[27].flDecayHFRatio = static_cast<float>(2 / 3);

  presets_room[28].flGain = 0.4f;
  presets_room[28].flDecayTime = 17.234f;
  presets_room[28].flDecayHFRatio = static_cast<float>(2 / 3);

  // Init each effect
  for (int i = 0; i < CSXROOM; ++i)
  {
    alReverbEffects[i] = al_context.createEffect();
    alReverbEffects[i].setReverbProperties(presets_room[i]);
  }

  alAuxEffectSlots = al_context.createAuxiliaryEffectSlot();
  alAuxEffectSlots.setGain(AL_REVERBMIX);
}

void SX_Shutdown(void)
{
  for (size_t i = 0; i < CSXROOM; ++i)
  {
    alReverbEffects[i].destroy();
  }
  alAuxEffectSlots.destroy();
}