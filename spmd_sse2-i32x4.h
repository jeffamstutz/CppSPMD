#pragma once

#include <emmintrin.h>

#include <cassert>

struct exec_t;
struct vbool;
struct vfloat;
struct vfloat_lref;
struct lint;

struct exec_t
{
    __m128 _mask;
};

exec_t operator&(const exec_t& a, const exec_t& b)
{
    return exec_t{ _mm_and_ps(a._mask, b._mask) };
}

exec_t operator~(const exec_t& a)
{
    return exec_t{ _mm_cmpeq_ps(a._mask, _mm_setzero_ps()) };
}

static exec_t exec = exec_t{ _mm_cmpeq_ps(_mm_setzero_ps(), _mm_setzero_ps()) };

struct vbool
{
    __m128 _value;
};

struct vfloat
{
    __m128 _value;

    vfloat& operator=(const vfloat& other)
    {
        _value = _mm_or_ps(_mm_and_ps(exec._mask, other._value), _mm_andnot_ps(exec._mask, _value));
        return *this;
    }
};

vfloat operator*(const vfloat& a, const vfloat& b)
{
    return vfloat{ _mm_mul_ps(a._value, b._value) };
}

vfloat sqrt(const vfloat& a)
{
    return vfloat{ _mm_sqrt_ps(a._value) };
}

vbool operator<(const vfloat& a, float b)
{
    return vbool{ _mm_cmplt_ps(a._value, _mm_set1_ps(b)) };
}

// reference to a vfloat stored linearly in memory
struct vfloat_ref
{
    float* _value;

    // scatter
    vfloat_ref& operator=(const vfloat& other)
    {
        int mask = _mm_movemask_ps(exec._mask);
        if (mask == 0b1111)
        {
            // "all on" optimization: vector store
            _mm_store_ps(_value, other._value);
        }
        else
        {
            // hand-written masked store
            __declspec(align(16)) float stored[4];
            _mm_store_ps(stored, other._value);

            for (int i = 0; i < 4; i++)
            {
                if (mask & (1 << i))
                    _value[i] = stored[i];
            }
        }
        return *this;
    }

    // gather
    operator vfloat() const
    {
        int mask = _mm_movemask_ps(exec._mask);
        if (mask == 0b1111)
        {
            // "all on" optimization: vector load
            return vfloat{ _mm_load_ps(_value) };
        }
        else
        {
            // hand-written masked load
            __declspec(align(16)) float loaded[4];
            for (int i = 0; i < 4; i++)
            {
                if (mask & (1 << i))
                    loaded[i] = _value[i];
            }

            return vfloat{ _mm_load_ps(loaded) };
        }
    }
};

struct lint
{
    __m128i _value;

    vfloat_ref operator[](float* ptr) const
    {
        return vfloat_ref{ ptr + _mm_cvtsi128_si32(_value) };
    }
};

lint operator+(const lint& a, int b)
{
    return lint{ _mm_add_epi32(a._value, _mm_set1_epi32(b)) };
}

lint operator+(int a, const lint& b)
{
    return lint{ _mm_add_epi32(_mm_set1_epi32(a), b._value) };
}

static const lint programIndex = lint{ _mm_set_epi32(3,2,1,0) };
static const int programCount = 4;

template<class IfBody>
void spmd_if(const vbool& cond, const IfBody& ifBody)
{
    // save old execution mask
    exec_t old_exec = exec;

    // apply "if" mask
    exec = exec & exec_t{ cond._value };

    // "all off" optimization
    int mask = _mm_movemask_ps(exec._mask);
    if (mask != 0)
    {
        ifBody();
    }

    // restore execution mask
    exec = old_exec;
}

template<class IfBody, class ElseBody>
void spmd_ifelse(const vbool& cond, const IfBody& ifBody, const ElseBody& elseBody)
{
    // save old execution mask
    exec_t old_exec = exec;

    // apply "if" mask
    exec = exec & exec_t{ cond._value };

    // "all off" optimization
    int mask = _mm_movemask_ps(exec._mask);
    if (mask != 0)
    {
        ifBody();
    }

    // invert mask for "else"
    exec = ~exec & old_exec;
    
    // "all off" optimization
    mask = _mm_movemask_ps(exec._mask);
    if (mask != 0)
    {
        elseBody();
    }

    // restore execution mask
    exec = old_exec;
}

template<class ForeachBody>
void spmd_foreach(int first, int last, const ForeachBody& foreachBody)
{
    // could allow this, just too lazy right now.
    assert(first <= last);

    // number of loops that don't require extra masking
    int numFullLoops = ((last - first) / programCount) * programCount;
    // number of loops that require extra masking (if loop count is not a multiple of programCount)
    int numPartialLoops = (last - first) % programCount;

    // do every loop that doesn't need to be masked
    __m128i loopIndex = _mm_add_epi32(programIndex._value, _mm_set1_epi32(first));
    for (int i = 0; i < numFullLoops; i += programCount)
    {
        foreachBody(lint{ loopIndex });
        loopIndex = _mm_add_epi32(loopIndex, _mm_set1_epi32(programCount));
    }

    // do a partial loop if necessary (if loop count is not a multiple of programCount)
    if (numPartialLoops > 0)
    {
        // save old execution mask
        exec_t old_exec = exec;

        // apply mask for partial loop
        exec = exec & exec_t{ _mm_castsi128_ps(_mm_cmplt_epi32(programIndex._value, _mm_set1_epi32(numPartialLoops))) };

        // do the partial loop
        foreachBody(lint{ loopIndex });

        // restore execution mask
        exec = old_exec;
    }
}