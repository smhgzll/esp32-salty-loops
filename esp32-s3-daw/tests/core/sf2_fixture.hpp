// SPDX-License-Identifier: GPL-3.0-or-later
// Three presets, two sharing one instrument; a large unused sample and a linked stereo pair.
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <vector>
inline std::vector<uint8_t> subsetFontFixture()
{
    using B = std::vector<uint8_t>;
    auto p16 = [](B &v, uint32_t n) { v.push_back(n); v.push_back(n>>8); };
    auto p32 = [&](B &v, uint32_t n) { p16(v,n); p16(v,n>>16); };
    auto name = [](B &v, const char *s) { size_t at=v.size(); v.resize(at+20); memcpy(v.data()+at,s,strlen(s)); };
    auto chunk = [&](const char *tag, const B &body) { B v(tag,tag+4); p32(v,body.size()); v.insert(v.end(),body.begin(),body.end()); if(body.size()&1)v.push_back(0); return v; };
    auto add = [](B &v, const B &x) { v.insert(v.end(),x.begin(),x.end()); };
    B pcm; const uint32_t starts[]={0,32814,34908}, lengths[]={32768,2048,2048};
    for(int s=0;s<3;s++) { for(uint32_t i=0;i<lengths[s];i++) p16(pcm,(uint16_t)(int16_t)lrint(12000*sin(2*M_PI*i/(s==2?64:128)))); for(int i=0;i<46;i++)p16(pcm,0); }
    B ph,pb,pm(10),pg,inst,ib,im(10),ig,sh;
    for(int i=0;i<4;i++) { name(ph,i==3?"EOP":i==0?"Unused":i==1?"Pair":"Shared kit");p16(ph,i==1?5:i==2?8:0);p16(ph,i==2?128:0);p16(ph,i);for(int k=0;k<3;k++)p32(ph,0);p16(pb,i);p16(pb,0);p16(pg,i==3?0:41);p16(pg,i==0?0:i==3?0:1); }
    for(int i=0;i<3;i++){name(inst,i==2?"EOI":i==0?"Unused":"Pair");p16(inst,i==2?3:i);}
    for(int i : {0,3,7,11}){p16(ib,i);p16(ib,0);}
    auto gen=[&](uint32_t op,uint32_t val){p16(ig,op);p16(ig,val);};
    gen(54,1);gen(58,60);gen(53,0);
    gen(43,63<<8);gen(54,1);gen(58,60);gen(53,1);
    gen(43,64|(127<<8));gen(54,1);gen(58,60);gen(53,2);gen(0,0);
    for(int i=0;i<4;i++){name(sh,i==3?"EOS":i==0?"Unused":i==1?"Left":"Right");for(int k=0;k<4;k++)p32(sh,i==3?0:starts[i]+((k==1||k==3)?lengths[i]:0));p32(sh,i==3?0:44100);sh.push_back(i==3?0:60);sh.push_back(0);p16(sh,i==1?2:i==2?1:0);p16(sh,i==3?0:i==0?1:i==1?4:2);}
    B pdta{'p','d','t','a'};
    for(auto x : {chunk("phdr",ph),chunk("pbag",pb),chunk("pmod",pm),chunk("pgen",pg),chunk("inst",inst),chunk("ibag",ib),chunk("imod",im),chunk("igen",ig),chunk("shdr",sh)})add(pdta,x);
    B sdta{'s','d','t','a'};add(sdta,chunk("smpl",pcm));B body{'s','f','b','k'};add(body,chunk("LIST",sdta));add(body,chunk("LIST",pdta));return chunk("RIFF",body);
}
