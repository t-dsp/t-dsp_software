typedef struct streamTypes_s {
  const char* name;
  OSCAudioBase* (*mkRoot)(const char*); //!< make object at root
  OSCAudioBase* (*mkGroup)(const char*,OSCAudioGroup&); //!< make object within group
} streamTypes_t;

void goFind(const char* addr);
