void prIndent(int indent)
{
  for (int i=0;i<indent;i++)
    Serial.print(' ');  
}


//=============================================================
void listObjects(OSCAudioBase* obj,int indent=0)
{
  static int depth = 0;
  depth++;
  while (depth < 5 && NULL != obj)
  {
    OSCAudioBase* grp = obj->getNextGroup();
    OSCAudioConnection* conn = obj->getFirstSrc();
    
    prIndent(indent);
    Serial.printf("%s is at %08X; route: %08X",obj->name,(uint32_t) obj,(uint32_t) obj->getNext()); Serial.flush();
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
      }
    }    
      
    //Serial.printf("group %08X\n",(uint32_t) (obj->getNextGroup()));
    Serial.flush();

    obj = obj->getNext();
  }
  depth--;
}


void listObjects(void)
{
  listObjects(OSCAudioBase::getFirst());
  Serial.println("--------------\n");
}

void listObjectsx(void){}

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
