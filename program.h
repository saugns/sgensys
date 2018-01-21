enum {
  MGS_TYPE_SOUND = 0,
  MGS_TYPE_ENV = 1
};

enum {
  MGS_FLAG_PLAY = 1<<0,
  MGS_FLAG_REFTIME = 1<<1,
  MGS_FLAG_REFPHASE = 1<<2,
  MGS_FLAG_ENTERED = 1<<3
};

enum {
  MGS_ATTR_FREQRATIO = 1<<0,
  MGS_ATTR_DYNFREQRATIO = 1<<1
};

enum {
  MGS_WAVE_SIN = 0,
  MGS_WAVE_SQR,
  MGS_WAVE_TRI,
  MGS_WAVE_SAW
};

enum {
  MGS_MODE_CENTER = 0,
  MGS_MODE_LEFT   = 1,
  MGS_MODE_RIGHT  = 2
};

enum {
  MGS_PMODS = 1<<0,
  MGS_FMODS = 1<<1,
  MGS_AMODS = 1<<2
};

typedef struct MGSProgramNodeChain {
  uint count;
  struct MGSProgramNode *chain;
} MGSProgramNodeChain;

typedef struct MGSProgramNode {
  struct MGSProgramNode *next, *ref;
  uchar type, flag, attr, wave, mode;
  float amp, dynamp, delay, time, freq, dynfreq, phase;
  uint id;
  MGSProgramNodeChain pmod, fmod, amod;
  struct MGSProgramNode *link;
} MGSProgramNode;

struct MGSProgram {
  MGSProgramNode *steps;
  uint stepc;
};
