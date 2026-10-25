/* vst2.h -- clean-room C declaration of the VST 2.4 plugin ABI: no vendored
 * SDK, values transcribed from a Nim port with known defects pre-fixed. */
#ifndef VST2_H
#define VST2_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define VST2_CDECL __cdecl
#elif defined(__i386__)
#define VST2_CDECL __attribute__((cdecl))
#else
#define VST2_CDECL
#endif

typedef intptr_t VstIntPtr;

struct AEffect;

/* opcode/index are always 32-bit in the real ABI; value and the return are
 * pointer-sized (VstIntPtr), e.g. effGetChunk returns a size through it. */
typedef VstIntPtr(VST2_CDECL *AudioMasterCallback)(struct AEffect *effect,
                                                   int32_t opcode,
                                                   int32_t index,
                                                   VstIntPtr value, void *ptr,
                                                   float opt);

typedef VstIntPtr(VST2_CDECL *AEffectDispatcherProc)(struct AEffect *effect,
                                                     int32_t opcode,
                                                     int32_t index,
                                                     VstIntPtr value, void *ptr,
                                                     float opt);

typedef void(VST2_CDECL *AEffectProcessProc)(struct AEffect *effect,
                                             float **inputs, float **outputs,
                                             int32_t sampleFrames);

typedef void(VST2_CDECL *AEffectProcessDoubleProc)(struct AEffect *effect,
                                                   double **inputs,
                                                   double **outputs,
                                                   int32_t sampleFrames);

typedef void(VST2_CDECL *AEffectSetParameterProc)(struct AEffect *effect,
                                                  int32_t index,
                                                  float parameter);

typedef float(VST2_CDECL *AEffectGetParameterProc)(struct AEffect *effect,
                                                   int32_t index);

enum
{
    kVstMagic = 0x56737450 /* 'VstP' */
};

typedef struct AEffect
{
    int32_t magic; /* must equal kVstMagic */
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process; /* deprecated, use processReplacing */
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags; /* effFlags* bitmask */
    VstIntPtr resvd1;
    VstIntPtr resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float ioRatio;
    void *object;
    void *user;
    int32_t uniqueID; /* four-char plugin id, host and plugin agree on it */
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56]; /* reserved padding to the real ABI's trailing bytes */
} AEffect;

/* opcode ordinals below are the ABI; positions must not be reordered */
enum VstDispatcherOpcode
{
    effOpen = 0,
    effClose,
    effSetProgram,
    effGetProgram,
    effSetProgramName,
    effGetProgramName,
    effGetParamLabel,
    effGetParamDisplay,
    effGetParamName,
    effGetVu,
    effSetSampleRate,
    effSetBlockSize,
    effMainsChanged,
    effEditGetRect,
    effEditOpen,
    effEditClose,
    effEditDraw,
    effEditMouse,
    effEditKey,
    effEditIdle,
    effEditTop,
    effEditSleep,
    effIdentify,
    effGetChunk,
    effSetChunk,
    effProcessEvents,
    effCanBeAutomated,
    effString2Parameter,
    effGetNumProgramCategories,
    effGetProgramNameIndexed,
    effCopyProgram,
    effConnectInput,
    effConnectOutput,
    effGetInputProperties,
    effGetOutputProperties,
    effGetPlugCategory,
    effGetCurrentPosition,
    effGetDestinationBuffer,
    effOfflineNotify,
    effOfflinePrepare,
    effOfflineRun,
    effProcessVarIo,
    effSetSpeakerArrangement,
    effSetBlockSizeAndSampleRate,
    effSetBypass,
    effGetEffectName,
    effGetErrorText,
    effGetVendorString,
    effGetProductString,
    effGetVendorVersion,
    effVendorSpecific,
    effCanDo,
    effGetTailSize,
    effIdle,
    effGetIcon,
    effSetViewPosition,
    effGetParameterProperties,
    effKeysRequired,
    effGetVstVersion,
    effEditKeyDown,
    effEditKeyUp,
    effSetEditKnobMode,
    effGetMidiProgramName,
    effGetCurrentMidiProgram,
    effGetMidiProgramCategory,
    effHasMidiProgramsChanged,
    effGetMidiKeyName,
    effBeginSetProgram,
    effEndSetProgram,
    effGetSpeakerArrangement,
    effShellGetNextPlugin,
    effStartProcess,
    effStopProcess,
    effSetTotalSampleToProcess,
    effSetPanLaw,
    effBeginLoadBank,
    effBeginLoadProgram,
    effSetProcessPrecision,
    effGetNumMidiInputChannels,
    effGetNumMidiOutputChannels
};

/* likewise: ordinals are the ABI the host dispatches audioMaster calls by */
enum VstMasterOpcode
{
    audioMasterAutomate = 0,
    audioMasterVersion,
    audioMasterCurrentId,
    audioMasterIdle,
    audioMasterPinConnected,
    audioMasterWantMidi,
    audioMasterGetTime,
    audioMasterProcessEvents,
    audioMasterSetTime,
    audioMasterTempoAt,
    audioMasterGetNumAutomatableParameters,
    audioMasterGetParameterQuantization,
    audioMasterIOChanged,
    audioMasterNeedIdle,
    audioMasterSizeWindow,
    audioMasterGetSampleRate,
    audioMasterGetBlockSize,
    audioMasterGetInputLatency,
    audioMasterGetOutputLatency,
    audioMasterGetPreviousPlug,
    audioMasterGetNextPlug,
    audioMasterWillReplaceOrAccumulate,
    audioMasterGetCurrentProcessLevel,
    audioMasterGetAutomationState,
    audioMasterOfflineStart,
    audioMasterOfflineRead,
    audioMasterOfflineWrite,
    audioMasterOfflineGetCurrentPass,
    audioMasterOfflineGetCurrentMetaPass,
    audioMasterSetOutputSampleRate,
    audioMasterGetOutputSpeakerArrangement,
    audioMasterGetVendorString,
    audioMasterGetProductString,
    audioMasterGetVendorVersion,
    audioMasterVendorSpecific,
    audioMasterSetIcon,
    audioMasterCanDo,
    audioMasterGetLanguage,
    audioMasterOpenWindow,
    audioMasterCloseWindow,
    audioMasterGetDirectory,
    audioMasterUpdateDisplay,
    audioMasterBeginEdit,
    audioMasterEndEdit,
    audioMasterOpenFileSelector,
    audioMasterCloseFileSelector,
    audioMasterEditFile,
    audioMasterGetChunkFile,
    audioMasterGetInputSpeakerArrangement
};

/* bit positions, not sequential values -- gaps are deprecated slots */
enum
{
    effFlagsHasEditor = 1 << 0,
    effFlagsHasClip = 1 << 1,
    effFlagsHasVu = 1 << 2,
    effFlagsCanMono = 1 << 3,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsExtIsAsync = 1 << 10,
    effFlagsExtHasBuffer = 1 << 11,
    effFlagsCanDoubleReplacing = 1 << 12
};

enum
{
    kPlugCategUnknown = 0,
    kPlugCategEffect,
    kPlugCategSynth
};

typedef struct VstEvent
{
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    char data[16];
} VstEvent;

typedef struct VstMidiEvent
{
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength;
    int32_t noteOffset;
    char midiData[4];
    int8_t detune;
    int8_t noteOffVelocity;
    int8_t reserved1;
    int8_t reserved2;
} VstMidiEvent;

typedef struct VstEvents
{
    int32_t numEvents;
    VstIntPtr reserved;
    /* real ABI is a flexible array; hosts index past 2 for numEvents > 2 */
    struct VstEvent *events[2];
} VstEvents;

enum
{
    kVstMidiType = 1
};

_Static_assert(sizeof(struct AEffect) == (sizeof(void *) == 4 ? 144 : 192),
               "AEffect size must be 144 (32-bit) or 192 (64-bit)");
_Static_assert(offsetof(struct AEffect, dispatcher) ==
                   (sizeof(void *) == 4 ? 4u : 8u),
               "dispatcher offset");
_Static_assert(offsetof(struct AEffect, flags) ==
                   (sizeof(void *) == 4 ? 36u : 56u),
               "flags offset");
_Static_assert(offsetof(struct AEffect, object) ==
                   (sizeof(void *) == 4 ? 64u : 96u),
               "object offset");
_Static_assert(offsetof(struct AEffect, uniqueID) ==
                   (sizeof(void *) == 4 ? 72u : 112u),
               "uniqueID offset");
_Static_assert(offsetof(struct AEffect, processReplacing) ==
                   (sizeof(void *) == 4 ? 80u : 120u),
               "processReplacing offset");
_Static_assert(offsetof(struct AEffect, future) ==
                   (sizeof(void *) == 4 ? 88u : 136u),
               "future offset");

#endif /* VST2_H */
