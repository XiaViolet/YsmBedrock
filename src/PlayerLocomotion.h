#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace ysm {
// Project a complete simulation step. Subtracting two float render positions
// instead can produce alternating zero and diagonal directions within one tick.
inline std::array<double,2> playerTravelAxes(std::array<float,2> previous,
    std::array<float,2> current, double yaw) {
    double dx=double(current[0])-previous[0], dz=double(current[1])-previous[1];
    double length=std::hypot(dx,dz);
    if(length<1e-4 || !std::isfinite(length) || !std::isfinite(yaw))return {};
    auto a=yaw*std::numbers::pi/180;
    double sine=std::sin(a), cosine=std::cos(a);
    auto spacing=[](float x){return double(std::nextafter(std::abs(x),std::numeric_limits<float>::infinity()))-std::abs(x);};
    // Both endpoints were rounded to Bedrock's float world coordinates.
    // If the signed component is smaller than their combined error, it is zero.
    double ex=(spacing(previous[0])+spacing(current[0]))*.5;
    double ez=(spacing(previous[1])+spacing(current[1]))*.5;
    auto project=[&](double x,double z){
        auto value=dx*x+dz*z;
        auto noise=ex*std::abs(x)+ez*std::abs(z)+length*1e-14;
        return std::abs(value)<=std::min(noise,length*.02) ? 0.0 : value/length;
    };
    return {project(-cosine,-sine),project(-sine,cosine)};
}

struct PlayerLocomotion {
    bool onGround{}, sprinting{};
    double groundSpeed{}, walkSpeed{};
    std::array<double,2> axes{};
};
}
