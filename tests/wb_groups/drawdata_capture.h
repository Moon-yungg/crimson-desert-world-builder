// Software capture of ACTUAL ImGui output, not a model of the editor layout.
// Font/texture bytes, raw vertices/indices/commands and viewport metadata accompany each BMP.
#pragma once
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace drawcapture {
struct Texture { int width, height; std::vector<unsigned char> rgba; };
inline void Bytes(const std::string& path, const void* bytes, size_t size) {
    std::ofstream f(path, std::ios::binary); f.write((const char*)bytes, size);
    if (!f) throw std::runtime_error("capture write failed: " + path);
}
inline void Save(const std::string& path, const ImDrawData& data, const std::map<ImTextureID, Texture>& textures) {
    const int w = (int)(data.DisplaySize.x * data.FramebufferScale.x), h = (int)(data.DisplaySize.y * data.FramebufferScale.y);
    if (!data.Valid || w <= 0 || h <= 0 || !data.TotalVtxCount) throw std::runtime_error("empty DrawData");
    std::vector<unsigned char> bmp((size_t)w*h*4, 0);
    std::ofstream raw(path + ".draw.json");
    raw << "{\"schema\":\"wb079.drawdata.v1\",\"producer\":\"production editor::Draw / ImGui::Render\",\"rasterizer\":\"tests/wb_groups/drawdata_capture.h\",\"sourceHashes\":\"runner result.json input manifest\",\"display\":[" << data.DisplayPos.x << ',' << data.DisplayPos.y << ',' << data.DisplaySize.x << ',' << data.DisplaySize.y << "],\"scale\":[" << data.FramebufferScale.x << ',' << data.FramebufferScale.y << "],\"textures\":[";
    bool first = true;
    for (const auto& entry : textures) {
        if (!first) raw << ','; first = false; const auto& t = entry.second;
        const std::string name = "texture-" + std::to_string((uint64_t)entry.first) + ".rgba";
        const auto slash = path.find_last_of("/\\"); Bytes(path.substr(0, slash + 1) + name, t.rgba.data(), t.rgba.size());
        raw << "{\"id\":" << (uint64_t)entry.first << ",\"width\":" << t.width << ",\"height\":" << t.height << ",\"file\":\"" << name << "\"}";
    }
    raw << "],\"lists\":[";
    auto cross = [](ImVec2 a, ImVec2 b, ImVec2 p) { return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x); };
    auto topLeft = [](ImVec2 a, ImVec2 b) { return a.y > b.y || (a.y == b.y && a.x < b.x); };
    for (int li=0; li<data.CmdListsCount; ++li) {
        const auto& list=*data.CmdLists[li]; if(li)raw<<','; raw<<"{\"vertices\":[";
        for(int i=0;i<list.VtxBuffer.Size;++i){if(i)raw<<',';const auto& v=list.VtxBuffer[i];raw<<'['<<v.pos.x<<','<<v.pos.y<<','<<v.uv.x<<','<<v.uv.y<<','<<v.col<<']';}
        raw<<"],\"indices\":[";for(int i=0;i<list.IdxBuffer.Size;++i){if(i)raw<<',';raw<<list.IdxBuffer[i];}raw<<"],\"commands\":[";
        for(int ci=0;ci<list.CmdBuffer.Size;++ci) {
            const auto& c=list.CmdBuffer[ci]; if(ci)raw<<',';
            raw<<"{\"elements\":"<<c.ElemCount<<",\"idxOffset\":"<<c.IdxOffset<<",\"vtxOffset\":"<<c.VtxOffset<<",\"texture\":"<<(uint64_t)c.GetTexID()<<",\"clip\":["<<c.ClipRect.x<<','<<c.ClipRect.y<<','<<c.ClipRect.z<<','<<c.ClipRect.w<<"]}";
            if(c.UserCallback) { if(c.UserCallback==ImDrawCallback_ResetRenderState)continue; throw std::runtime_error("unhandled capture callback"); }
            auto tex=textures.find(c.GetTexID()); if(tex==textures.end())throw std::runtime_error("unregistered DrawData texture"); const auto& t=tex->second;
            auto screen=[&](ImVec2 p){return ImVec2((p.x-data.DisplayPos.x)*data.FramebufferScale.x,(p.y-data.DisplayPos.y)*data.FramebufferScale.y);};
            const auto clip0=screen({c.ClipRect.x,c.ClipRect.y}), clip1=screen({c.ClipRect.z,c.ClipRect.w});
            for(unsigned i=0;i+2<c.ElemCount;i+=3) {
                ImDrawVert v[3]; ImVec2 p[3]; for(int k=0;k<3;++k){v[k]=list.VtxBuffer[c.VtxOffset+list.IdxBuffer[c.IdxOffset+i+k]];p[k]=screen(v[k].pos);}
                float area=cross(p[0],p[1],p[2]);if(area==0)continue;if(area<0){std::swap(p[1],p[2]);std::swap(v[1],v[2]);area=-area;}
                int x0=std::max(0,(int)std::floor(std::max(clip0.x,std::min({p[0].x,p[1].x,p[2].x}))));
                int y0=std::max(0,(int)std::floor(std::max(clip0.y,std::min({p[0].y,p[1].y,p[2].y}))));
                int x1=std::min(w,(int)std::ceil(std::min(clip1.x,std::max({p[0].x,p[1].x,p[2].x}))));
                int y1=std::min(h,(int)std::ceil(std::min(clip1.y,std::max({p[0].y,p[1].y,p[2].y}))));
                for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
                    const ImVec2 q(x+.5f,y+.5f);if(q.x<clip0.x||q.y<clip0.y||q.x>=clip1.x||q.y>=clip1.y)continue;
                    float b[3]={cross(p[1],p[2],q),cross(p[2],p[0],q),cross(p[0],p[1],q)};bool inside=true;
                    for(int k=0;k<3;++k)if(b[k]<0||(b[k]==0&&!topLeft(p[(k+1)%3],p[(k+2)%3])))inside=false;
                    if(!inside)continue;for(float& z:b)z/=area;
                    float u=0,vv=0,col[4]={};for(int k=0;k<3;++k){u+=b[k]*v[k].uv.x;vv+=b[k]*v[k].uv.y;for(int ch=0;ch<4;++ch)col[ch]+=b[k]*((v[k].col>>(8*ch))&255);}
                    // Bilinear atlas/texture sampling matches the production renderer's linear sampler.
                    float tx=u*t.width-.5f,ty=vv*t.height-.5f;int ix=(int)std::floor(tx),iy=(int)std::floor(ty);float fx=tx-ix,fy=ty-iy,tc[4]={};
                    for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){size_t at=((size_t)std::clamp(iy+dy,0,t.height-1)*t.width+std::clamp(ix+dx,0,t.width-1))*4;float a=(dx?fx:1-fx)*(dy?fy:1-fy);for(int ch=0;ch<4;++ch)tc[ch]+=t.rgba[at+ch]*a;}
                    float alpha=col[3]*tc[3]/65025.f;size_t out=((size_t)y*w+x)*4;
                    for(int ch=0;ch<3;++ch){int bgra=2-ch;float src=col[ch]*tc[ch]/255.f;bmp[out+bgra]=(unsigned char)std::clamp(src*alpha+bmp[out+bgra]*(1-alpha)+.5f,0.f,255.f);}bmp[out+3]=255;
                }
            }
        }raw<<"]}";
    }raw<<"]}";if(!raw)throw std::runtime_error("raw DrawData write failed");
    // Uncompressed top-down BGRA BMP (no platform/GPU dependency).
    std::vector<unsigned char> header(54,0);auto put=[&](int at,uint32_t n,int count){for(int i=0;i<count;++i)header[at+i]=(unsigned char)(n>>(8*i));};
    header[0]='B';header[1]='M';put(2,(uint32_t)(54+bmp.size()),4);put(10,54,4);put(14,40,4);put(18,w,4);put(22,(uint32_t)-h,4);put(26,1,2);put(28,32,2);
    std::ofstream f(path+".bmp",std::ios::binary);f.write((char*)header.data(),header.size());f.write((char*)bmp.data(),bmp.size());if(!f)throw std::runtime_error("BMP write failed");
}
}
