#pragma once

#if defined(TORIRENDER_USE_OPENACC) && (defined(__NVCOMPILER) || defined(__PGI))
#define TORIRENDER_ACC_ROUTINE_SEQ _Pragma("acc routine seq")
#else
#define TORIRENDER_ACC_ROUTINE_SEQ
#endif
