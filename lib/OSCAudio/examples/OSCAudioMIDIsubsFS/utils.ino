void prIndent(int indent)
{
  for (int i=0;i<indent;i++)
    Serial.print(' ');  
}

void dbgMatch(char* addr,char* patt,int indent)
{
  if (0 == indent)
    Serial.printf("Match %s to message pattern %s\n",addr,patt);  
  else
  {
    prIndent(indent);
    Serial.printf("%s,%s\n",addr,patt);  
  }
}


//=============================================================
#define DUMP_LEN 16
void memDump(char* p, int n)
{
  // copy to stack to avoid corruption
  char* q=alloca(n);
  memcpy(q,p,n);
  
  for (int i=0;i<n/DUMP_LEN;i++)
  {
    char buf[100];
    int off = sprintf(buf,"%08lX: ",(uint32_t) p);
    for (int j=0;j<DUMP_LEN;j++)
      off += sprintf(buf+off,"%02X ",q[j]);
    off += sprintf(buf+off," ");
    for (int j=0;j<DUMP_LEN;j++)
    {
      char c = q[j];
      if (c<32 || c> 126) c = '.';
      off += sprintf(buf+off,"%c",c);
    }

    Serial.println(buf);
    p += DUMP_LEN;
    q += DUMP_LEN;
  }
  Serial.println();
}


//=============================================================
#define ZAP_MAX 12
#define ZAP_DEPTH 26
void zapHeap(void)
{
  static int depth = 0;
  static char co = 'a';
  int rs = 1;
  void* rm = malloc(rs);
  void* rm2 = rm;
  do
  {
    free(rm2);
    rm2 = malloc(++rs);  
  } while (rs <= ZAP_MAX && rm2 == rm);
  free(rm2);
  rs--;
  rm = malloc(rs);
  for (int i=0;i<rs;i++)
    *((char*) rm+i) = depth+co;
  //Serial.printf("%d bytes at %08X; depth %d\n",rs,(uint32_t) rm,depth);
  if (/*rs <ZAP_MAX &&*/ depth < ZAP_DEPTH)
  {
    ++depth;
    zapHeap();
    --depth;
  }
  free(rm);
  if (0 == depth)
    co = 'A'+'a'-co;
}


#define LO_MAX_COUNT 1000000
//=============================================================
void listObjects(OSCAudioBase* obj,int indent=0)
{
  static bool locked = false;
  static int count = 0;
  static int depth = 0;
  depth++;
  while (depth < 5 && NULL != obj && !locked)
  {
    OSCAudioBase* grp = obj->getNextGroup();
    OSCAudioConnection* conn = obj->getFirstSrc();
    
    prIndent(indent);
    Serial.printf("%s is at %08X (%08X); route: %08X",obj->name,(uint32_t) obj,(uint32_t) obj->sibling,(uint32_t) obj->getNext()); Serial.flush();
    count++; if (count>LO_MAX_COUNT) {locked = true; break;}
    if (NULL != grp)
    {
      Serial.printf("; group: %08X\n",(uint32_t) grp); Serial.flush();
      listObjects(grp,indent+2);
    }
    else
      Serial.println(); Serial.flush();   

    if (NULL != conn)
    {
      prIndent(indent+2);
      Serial.println("source connections:");
      while (NULL != conn)
      {
        prIndent(indent+4);
        Serial.println(conn->name);
        conn = conn->getNextSrc();
        count++; if (count>LO_MAX_COUNT) {locked = true; break;}
      }
    }
    
    conn = obj->getFirstDst();
    if (NULL != conn)
    {
      prIndent(indent+2);
      Serial.println("destination connections:");
      while (NULL != conn)
      {
        prIndent(indent+4);
        Serial.println(conn->name);
        conn = conn->getNextDst();
        count++; if (count>LO_MAX_COUNT) {locked = true; break;}
      }
    }    
      
    //Serial.printf("group %08X\n",(uint32_t) (obj->getNextGroup()));
    Serial.flush();

    obj = obj->getNext();
  }
  depth--;
  if (0 == depth && !locked)
    count = 0;
  //Serial.printf("Count: %d\n",count);
}


void listObjects(bool doit)
{
  if (doit)
  {
    listObjects(OSCAudioBase::getFirst());
    Serial.println("--------------\n");
  }
}


//=============================================================
void goFind(OSCMessage& msg,int o,int md,OSCAudioBase* ooi)
{
  int o2;

  while (NULL != ooi)
  {
    for (int i=1;i<o;i++) Serial.print(' ');
    Serial.printf("%s at %08X",ooi->name,ooi);
    o2 = msg.match(ooi->name,o);
    if (o2 > 0)
    {
      if (o2+o == md)
        Serial.println("...hit!");
      else
      {
        Serial.println();
        goFind(msg,o+o2,md,ooi->getNextGroup()); 
        Serial.println("urn"); Serial.flush();
      }
    }
    else
      Serial.println();
    ooi = ooi->getNext();
    //Serial.println((uint32_t) ooi,HEX); Serial.flush();
  }
  Serial.print("ret"); Serial.flush();
}

void goFind(const char* addr)
{
  OSCMessage msg(addr);
  int md = strlen(addr);
  Serial.printf("done after %d\n",md);
  //msg.add;
  OSCAudioBase* ooi = OSCAudioBase::getFirst();

  goFind(msg,0,md,ooi);
  Serial.println("urn");
  
}

void listAllTypes(void)
{
    int objIdx;
    
    for (objIdx = OSCAudioBase::countOfAudioTypes()-1;objIdx>=0;objIdx--)
      Serial.println(OSCAudioBase::audioTypes[objIdx].name);
}
