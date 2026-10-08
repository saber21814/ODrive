#pragma once
#include "Rehab/core/engine.hpp"
namespace rehab { namespace wire {
size_t config_field_count();
const char* config_field_name(uint16_t);
Result set_config_field(Config&,uint16_t id,uint16_t type,uint32_t bits);
bool get_config_field(const Config&,uint16_t id,uint16_t& type,uint32_t& bits);
}}
