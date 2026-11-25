#pragma once
#include <cstddef>
#include <cstdint>
extern "C" {
struct OptPlamo;
using OptPlamoRead = std::size_t (*)(void*,std::uint64_t,std::uint8_t*,std::size_t);
using OptPlamoCancel = std::int32_t (*)(void*);
using OptPlamoDelta = std::int32_t (*)(void*,const std::uint8_t*,std::size_t);
OptPlamo* opt_plamo_open(std::size_t,std::size_t,OptPlamoRead,void*);
void opt_plamo_close(OptPlamo*);
std::size_t opt_plamo_max_piece(const OptPlamo*);
std::int32_t opt_plamo_tokenize(const OptPlamo*,const std::uint8_t*,std::size_t,std::int32_t,std::int32_t,std::int32_t*,std::size_t);
std::int32_t opt_plamo_generate(const OptPlamo*,const std::int32_t*,std::size_t,std::size_t,OptPlamoCancel,OptPlamoDelta,void*);
}
