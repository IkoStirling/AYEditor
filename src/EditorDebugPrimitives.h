#pragma once
#include <AYPhysics/PhysicsDebugDraw.h>
#include <AYMath/MathTypes.h>
#include <array>
namespace ayt::editor::debug_detail {
using Record=ayt::physics::PhysicsDebugRecord;using Category=ayt::physics::PhysicsDebugCategory;using V=ayt::math::FVector3;
inline constexpr uint32_t green=0xff00ff00,red=0xff0000ff,cyan=0xffffff00,yellow=0xff00ffff;
struct Collector : ayt::physics::PhysicsDebugSnapshot {
    std::array<size_t,ayt::physics::kPhysicsDebugCategoryCount> counts{};
    std::array<size_t,ayt::physics::kPhysicsDebugCategoryCount> omitted{};
    bool hasRoom(Category category)const{return counts[size_t(category)]<127;}
    bool append(Record record){const auto category=size_t(record.category);if(category>=counts.size() || counts[category]>=127){++droppedRecords;if(category<omitted.size())++omitted[category];return false;}if(!ayt::physics::PhysicsDebugSnapshot::append(std::move(record)))return false;++counts[category];return true;}
    std::vector<Record> finish(){for(size_t i=0;i<omitted.size();++i)if(omitted[i]){Record r;r.category=static_cast<Category>(i);r.label="Debug adapter budget";r.details="Omitted at least "+std::to_string(omitted[i])+" rows; per-category content budget is127 plus this notice.";ayt::physics::PhysicsDebugSnapshot::append(std::move(r));}return std::move(records);}
};
inline void marker(Record& row,V p,uint32_t color=cyan,float size=.12f){row.anchor=p;row.lines.push_back({p-V{size,0,0},p+V{size,0,0},color});row.lines.push_back({p-V{0,size,0},p+V{0,size,0},color});}
inline void box(Record& row,V lo,V hi,uint32_t color=cyan){if(lo.z==hi.z){V p[]={{lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{lo.x,hi.y,lo.z}};for(int i=0;i<4;++i)row.lines.push_back({p[i],p[(i+1)%4],color});return;}V points[8];for(int i=0;i<8;++i)points[i]={i&1?hi.x:lo.x,i&2?hi.y:lo.y,i&4?hi.z:lo.z};for(int i=0;i<8;++i)for(int bit=1;bit<8;bit*=2)if(!(i&bit))row.lines.push_back({points[i],points[i|bit],color});}
inline void screenRect(Record& row,const ayt::math::FRectangle& r,uint32_t color){V points[]={{r.minX,r.minY,0},{r.maxX,r.minY,0},{r.maxX,r.maxY,0},{r.minX,r.maxY,0}};for(int i=0;i<4;++i)row.screenLines.push_back({points[i],points[(i+1)%4],color});}
inline bool enabled(uint32_t mask,Category category){return (mask&(1u<<uint32_t(category)))!=0;}
}
