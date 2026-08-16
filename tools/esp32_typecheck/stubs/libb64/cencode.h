#pragma once
#include <cstddef>
typedef enum { step_A, step_B, step_C } base64_encodestep;
typedef struct { base64_encodestep step; char result; int stepcount; } base64_encodestate;
extern "C" {
void base64_init_encodestate(base64_encodestate* state);
int base64_encode_block(const char* in, int len, char* out, base64_encodestate* state);
int base64_encode_blockend(char* out, base64_encodestate* state);
}
