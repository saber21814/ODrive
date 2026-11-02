#pragma once
#include <stdint.h>
extern unsigned mock_critical_depth;
struct MockCritical { MockCritical(){++mock_critical_depth;} ~MockCritical(){--mock_critical_depth;} explicit operator bool()const{return true;} };
#define CRITICAL_SECTION() if (MockCritical mock_critical{})
