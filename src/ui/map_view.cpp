// Native OpenStreetMap raster view. Only visible tiles are requested; decoding never waits on HTTP.
// GPL-3.0-or-later.
#include "map_view.h"
#include "imgui.h"
#include <GLFW/glfw3.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <chrono>
#include <ctime>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 256
#include "stb_image.h"

namespace droneid::ui {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr size_t max_bytes = 1024*1024;
struct TileKey {
  int z,x,y;
  bool operator<(const TileKey& b) const { return std::tie(z,x,y)<std::tie(b.z,b.x,b.y); }
  std::string path() const { return std::to_string(z)+"/"+std::to_string(x)+"/"+std::to_string(y)+".png"; }
};
fs::path cache_root() {
#ifdef _WIN32
  const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
  if (base) return fs::path(base)/L"punjdrone"/L"osm-tiles";
#else
  if (const char* base=std::getenv("XDG_CACHE_HOME")) return fs::path(base)/"punjdrone/osm-tiles";
  if (const char* base=std::getenv("HOME")) return fs::path(base)/".cache/punjdrone/osm-tiles";
#endif
  return fs::temp_directory_path()/"punjdrone-osm-tiles";
}
std::vector<unsigned char> read_cache(const fs::path& p) {
  std::error_code ec;
  const auto size=fs::file_size(p,ec);
  if(ec || !size || size>max_bytes) return {};
  std::vector<unsigned char> data(static_cast<size_t>(size));
  std::ifstream in(p,std::ios::binary);
  if(!in.read(reinterpret_cast<char*>(data.data()),data.size())) return {};
  return data;
}
std::string modified_since(fs::file_time_type time) {
  const auto sys=std::chrono::system_clock::now()+(time-fs::file_time_type::clock::now());
  const std::time_t t=std::chrono::system_clock::to_time_t(sys);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc,&t);
#else
  gmtime_r(&t,&utc);
#endif
  char buf[80]{};
  std::strftime(buf,sizeof buf,"%a, %d %b %Y %H:%M:%S GMT",&utc);
  return buf;
}
struct HttpResult { long status=0; std::vector<unsigned char> bytes; };
HttpResult fetch(const std::string& path, const std::string& since) {
  HttpResult result;
#ifdef _WIN32
  HINTERNET session=WinHttpOpen(L"PunjDrone/1.0 (native packet map)",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
  if(!session) return result;
  WinHttpSetTimeouts(session,2000,2000,2000,2000);
  HINTERNET connection=WinHttpConnect(session,L"tile.openstreetmap.org",INTERNET_DEFAULT_HTTPS_PORT,0);
  std::wstring target(path.begin(),path.end()); target=L"/"+target;
  HINTERNET request=connection ? WinHttpOpenRequest(connection,L"GET",target.c_str(),nullptr,
                    WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE) : nullptr;
  if(request) {
    std::string header=since.empty()?"":"If-Modified-Since: "+since+"\r\n";
    std::wstring wide(header.begin(),header.end());
    if(WinHttpSendRequest(request,wide.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:wide.c_str(),
                         static_cast<DWORD>(wide.size()),WINHTTP_NO_REQUEST_DATA,0,0,0) && WinHttpReceiveResponse(request,nullptr)) {
      DWORD code=0,len=sizeof code;
      WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&len,WINHTTP_NO_HEADER_INDEX);
      result.status=code;
      auto deadline=Clock::now()+std::chrono::seconds(5);
      for(;;) {
        unsigned char buffer[16384]; DWORD count=0;
        if(Clock::now()>deadline || !WinHttpReadData(request,buffer,sizeof buffer,&count)) { result.status=0; break; }
        if(!count) break;
        if(result.bytes.size()+count>max_bytes) { result.status=0; break; }
        result.bytes.insert(result.bytes.end(),buffer,buffer+count);
      }
    }
    WinHttpCloseHandle(request);
  }
  if(connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
#else
  CURL* c=curl_easy_init();
  if(!c) return result;
  const std::string url="https://tile.openstreetmap.org/"+path;
  curl_easy_setopt(c,CURLOPT_URL,url.c_str());
  curl_easy_setopt(c,CURLOPT_USERAGENT,"PunjDrone/1.0 (native packet map)");
  curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT_MS,2000L);
  curl_easy_setopt(c,CURLOPT_TIMEOUT_MS,5000L);
  curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,+[](char* bytes,size_t size,size_t n,void* ctx)->size_t {
    auto& out=*static_cast<std::vector<unsigned char>*>(ctx);
    const size_t count=size*n;
    if(count>max_bytes-out.size()) return 0;
    out.insert(out.end(),bytes,bytes+count); return count;
  });
  curl_easy_setopt(c,CURLOPT_WRITEDATA,&result.bytes);
  curl_slist* headers=nullptr;
  if(!since.empty()) headers=curl_slist_append(headers,("If-Modified-Since: "+since).c_str());
  if(headers) curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
  if(curl_easy_perform(c)==CURLE_OK) curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&result.status);
  curl_slist_free_all(headers); curl_easy_cleanup(c);
#endif
  return result;
}
std::vector<unsigned char> decode(const std::vector<unsigned char>& data) {
  if(data.empty()) return {};
  int w=0,h=0,n=0;
  if(!stbi_info_from_memory(data.data(),static_cast<int>(data.size()),&w,&h,&n) || w!=256 || h!=256) return {};
  unsigned char* p=stbi_load_from_memory(data.data(),static_cast<int>(data.size()),&w,&h,&n,4);
  if(!p) return {};
  std::vector<unsigned char> rgba(p,p+256*256*4); stbi_image_free(p); return rgba;
}
}
struct MapView::Impl {
  struct Image { TileKey key; std::vector<unsigned char> rgba; };
  struct Texture { GLuint id=0; uint64_t used=0; };
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<TileKey> pending;
  std::deque<Image> ready;
  std::set<TileKey> loading;
  std::map<TileKey,Clock::time_point> retry;
  std::map<TileKey,Texture> textures;
  std::atomic<bool> stop{false};
  bool online = std::getenv("DRONEID_MAP_OFFLINE")==nullptr;
  uint64_t frame=0;
  fs::path cache;
  std::string status="Loading map...";
  std::thread worker;
  Impl() {
    try { cache=cache_root(); } catch(...) { status="Map cache unavailable"; }
#ifndef _WIN32
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    worker=std::thread([this]{run();});
  }
  ~Impl() {
    stop=true; cv.notify_one(); if(worker.joinable()) worker.join();
    for(auto& kv:textures) glDeleteTextures(1,&kv.second.id);
#ifndef _WIN32
    curl_global_cleanup();
#endif
  }
  void run() {
    while(!stop) {
      TileKey key; bool network;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock,[this]{return stop || !pending.empty();});
        if(stop) break;
        key=pending.front(); pending.pop_front(); loading.insert(key); network=online;
      }
      std::vector<unsigned char> rgba;
      std::string message;
      try {
        const fs::path file=cache/key.path();
        auto bytes=cache.empty()?std::vector<unsigned char>{}:read_cache(file);
        std::error_code ec;
        auto modified=fs::last_write_time(file,ec);
        const bool fresh=!ec && fs::file_time_type::clock::now()-modified<std::chrono::hours(24*7);
        if(!bytes.empty()) rgba=decode(bytes);
        if(network && (!fresh || rgba.empty())) {
          auto response=fetch(key.path(),rgba.empty()?"":modified_since(modified));
          if(response.status==304 && !rgba.empty()) fs::last_write_time(file,fs::file_time_type::clock::now(),ec);
          else if(response.status==200) {
            auto decoded=decode(response.bytes);
            if(!decoded.empty()) {
              rgba=std::move(decoded);
              if(!cache.empty()) {
                fs::create_directories(file.parent_path(),ec);
                auto temporary=file;
                temporary += ".partial";
                std::ofstream out(temporary,std::ios::binary);
                out.write(reinterpret_cast<const char*>(response.bytes.data()),response.bytes.size());
                out.close();
                if(out) { fs::remove(file,ec); fs::rename(temporary,file,ec); }
              }
            } else message="Invalid map tile; retry in 60s";
          } else message="Map network unavailable; cached tiles retained";
        }
        if(!network && rgba.empty()) message="Offline: this area is not cached";
      } catch(...) { message="Map cache/network error; receiver unaffected"; }
      {
        std::lock_guard<std::mutex> lock(mutex);
        loading.erase(key);
        if(rgba.empty()) retry[key]=Clock::now()+std::chrono::seconds(60);
        else { ready.push_back({key,std::move(rgba)}); if(ready.size()>32) ready.pop_front(); }
        status=message.empty()?"Drag to pan | scroll or +/- to zoom":message;
      }
    }
  }
  void poll() {
    std::lock_guard<std::mutex> lock(mutex);
    for(auto& image:ready) {
      auto& t=textures[image.key];
      if(!t.id) glGenTextures(1,&t.id);
      glBindTexture(GL_TEXTURE_2D,t.id);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,image.rgba.data());
      t.used=frame;
    }
    ready.clear();
    while(textures.size()>96) {
      auto oldest=std::min_element(textures.begin(),textures.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
      glDeleteTextures(1,&oldest->second.id); textures.erase(oldest);
    }
  }
};
MapView::MapView():impl_(std::make_unique<Impl>()) {}
MapView::~MapView()=default;
void MapView::draw(PacketSelection& s) {
  auto& m=*impl_; ++m.frame; m.poll();
  ImGui::SeparatorText("Packet location / OpenStreetMap");
  if(ImGui::SmallButton("Punjab")) { s.lat=kPunjabLat;s.lon=kPunjabLon;s.zoom=7; }
  ImGui::SameLine(); if(ImGui::SmallButton("+")) s.zoom=std::min(19,s.zoom+1);
  ImGui::SameLine(); if(ImGui::SmallButton("-")) s.zoom=std::max(2,s.zoom-1);
  ImGui::SameLine();
  bool online; {std::lock_guard<std::mutex> lock(m.mutex); online=m.online;}
  if(ImGui::Checkbox("Online tiles",&online)) {std::lock_guard<std::mutex> lock(m.mutex);m.online=online;m.retry.clear();}
  if(ImGui::IsItemHovered()) ImGui::SetTooltip("Fetch visible map tiles from OpenStreetMap. The service sees the viewed area, not packet contents. Turn off for cached tiles only.");
  ImGui::TextDisabled("%.5f, %.5f | zoom %d",s.lat,s.lon,s.zoom);
  const ImVec2 origin=ImGui::GetCursorScreenPos();
  const ImVec2 size(std::max(32.f,ImGui::GetContentRegionAvail().x),std::max(90.f,ImGui::GetContentRegionAvail().y-45));
  ImGui::InvisibleButton("map_canvas",size,ImGuiButtonFlags_MouseButtonLeft);
  if(ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel!=0) s.zoom=std::clamp(s.zoom+(ImGui::GetIO().MouseWheel>0?1:-1),2,19);
  auto center=project_location(s.lat,s.lon,s.zoom);
  if(ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    auto ll=unproject_location(center.x-ImGui::GetIO().MouseDelta.x,center.y-ImGui::GetIO().MouseDelta.y,s.zoom);
    s.lat=ll.x; s.lon=ll.y; center=project_location(s.lat,s.lon,s.zoom);
  }
  const double left=center.x-size.x/2,top=center.y-size.y/2;
  auto* draw=ImGui::GetWindowDrawList();
  draw->PushClipRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),true);
  draw->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(28,39,49,255));
  std::vector<TileKey> visible;
  const int count=1<<s.zoom;
  for(int y=static_cast<int>(std::floor(top/256));y<=static_cast<int>(std::floor((top+size.y)/256));++y) {
    if(y<0 || y>=count) continue;
    for(int x=static_cast<int>(std::floor(left/256));x<=static_cast<int>(std::floor((left+size.x)/256));++x) {
      TileKey key{s.zoom,(x%count+count)%count,y}; visible.push_back(key);
      ImVec2 a(origin.x+static_cast<float>(x*256.-left),origin.y+static_cast<float>(y*256.-top)),b(a.x+256,a.y+256);
      auto t=m.textures.find(key);
      if(t!=m.textures.end()) {t->second.used=m.frame;draw->AddImage(static_cast<ImTextureID>(t->second.id),a,b);}
      else {draw->AddRect(a,b,IM_COL32(50,65,78,255));draw->AddText(ImVec2(a.x+12,a.y+12),IM_COL32(145,160,172,255),online?"Loading map...":"Tile not cached");}
    }
  }
  auto marker=[&](double lat,double lon,ImU32 color,const char* label,float label_y) {
    auto pos=project_location(lat,lon,s.zoom);
    double dx=pos.x-center.x,world=256.*count;
    if(dx>world/2) dx-=world;
    if(dx<-world/2) dx+=world;
    ImVec2 p(origin.x+size.x/2+static_cast<float>(dx),origin.y+size.y/2+static_cast<float>(pos.y-center.y));
    draw->AddCircleFilled(p,8,IM_COL32(255,255,255,255));draw->AddCircleFilled(p,5,color);
    const auto text_size=ImGui::CalcTextSize(label);
    draw->AddLine(p,ImVec2(p.x+10,p.y+label_y+10),color);
    draw->AddRectFilled(ImVec2(p.x+10,p.y+label_y),ImVec2(p.x+16+text_size.x,p.y+label_y+20),IM_COL32(15,23,31,230),3);
    draw->AddText(ImVec2(p.x+13,p.y+label_y+3),color,label);
  };
  if(s.packet && s.packet->crc_ok && s.packet->telemetry_parsed) {
    const auto& p=*s.packet;
    if(p.pilot_valid && valid_location(p.pilot_lat,p.pilot_lon)) marker(p.pilot_lat,p.pilot_lon,IM_COL32(66,174,255,255),"Pilot/operator",-32);
    if(p.home_valid && valid_location(p.home_lat,p.home_lon)) marker(p.home_lat,p.home_lon,IM_COL32(70,220,128,255),"Home",34);
    if(packet_drone_location(p)) marker(p.drone_lat,p.drone_lon,IM_COL32(244,79,69,255),"Drone",8);
  }
  draw->PopClipRect();
  {
    std::lock_guard<std::mutex> lock(m.mutex);
    m.pending.clear(); // discard requests for viewports the operator has left
    for(const auto& key:visible) if(!m.textures.count(key) && !m.loading.count(key) &&
       (!m.retry.count(key) || Clock::now()>=m.retry[key])) m.pending.push_back(key);
    ImGui::TextWrapped("%s",m.status.c_str());
    if(m.retry.size()>256) m.retry.clear();
  }
  m.cv.notify_one();
  ImGui::TextLinkOpenURL("(c) OpenStreetMap contributors","https://www.openstreetmap.org/copyright");
}
}
