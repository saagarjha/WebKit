
/*
 * Copyright (c) 2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/FixedVector.h>

namespace WGSL {

// A constant value might be:
// - a scalar
// - a vector
// - a matrix
// - a fixed-size array type
// - a structure
struct ConstantValue;

struct ConstantArray {
    ConstantArray(size_t size)
        : elements(size)
    {
    }

    ConstantArray(FixedVector<ConstantValue>&& elements)
        : elements(WTFMove(elements))
    {
    }

    FixedVector<ConstantValue> elements;
};

struct ConstantVector {
    ConstantVector(size_t size)
        : elements(size)
    {
    }

    ConstantVector(FixedVector<ConstantValue>&& elements)
        : elements(WTFMove(elements))
    {
    }

    FixedVector<ConstantValue> elements;
};

struct ConstantMatrix {
    ConstantMatrix(uint32_t columns, uint32_t rows)
        : columns(columns)
        , rows(rows)
        , elements(columns * rows)
    {
    }

    ConstantMatrix(uint32_t columns, uint32_t rows, const FixedVector<ConstantValue>& elements)
        : columns(columns)
        , rows(rows)
        , elements(elements)
    {
        RELEASE_ASSERT(elements.size() == columns * rows);
    }

    uint32_t columns;
    uint32_t rows;
    FixedVector<ConstantValue> elements;
};

using BaseValue = std::variant<float, double, int32_t, uint32_t, int64_t, bool, ConstantArray, ConstantVector, ConstantMatrix>;
struct ConstantValue : BaseValue {
    ConstantValue() = default;

    using BaseValue::BaseValue;

    void dump(PrintStream&) const;

    bool isBool() const { return std::holds_alternative<bool>(*this); }
    bool isI32() const { return std::holds_alternative<int32_t>(*this); }
    bool isU32() const { return std::holds_alternative<uint32_t>(*this); }
    bool isAbstractInt() const { return std::holds_alternative<int64_t>(*this); }
    bool isF32() const { return std::holds_alternative<float>(*this); }
    bool isAbstractFloat() const { return std::holds_alternative<double>(*this); }
    bool isVector() const { return std::holds_alternative<ConstantVector>(*this); }
    bool isMatrix() const { return std::holds_alternative<ConstantMatrix>(*this); }
    bool isArray() const { return std::holds_alternative<ConstantArray>(*this); }

    bool toBool() const { return std::get<bool>(*this); }
    float toF32() const { return std::get<float>(*this); }

    int64_t integerValue() const
    {
        if (auto* i32 = std::get_if<int32_t>(this))
            return *i32;
        if (auto* u32 = std::get_if<uint32_t>(this))
            return *u32;
        if (auto* abstractInt = std::get_if<int64_t>(this))
            return *abstractInt;
        RELEASE_ASSERT_NOT_REACHED();
    }

    const ConstantVector& toVector() const
    {
        return std::get<ConstantVector>(*this);
    }
};

template<typename To, typename From>
std::optional<To> convertInteger(From value)
{
    auto result = Checked<To, RecordOverflow>(value);
    if (UNLIKELY(result.hasOverflowed()))
        return std::nullopt;
    return { result.value() };
}

template<typename To, typename From>
std::optional<To> convertFloat(From value)
{
    static_assert(std::is_floating_point<To>::value || std::is_same<To, __fp16>::value, "Result type is expected to be a floating point type: double, float, or __fp16");
    if (value > std::numeric_limits<To>::max())
        return std::nullopt;
    if (value < std::numeric_limits<To>::lowest())
        return std::nullopt;
    if (std::abs(value) < std::numeric_limits<To>::min())
        return { 0 };

    return { value };
}

} // namespace WGSL
