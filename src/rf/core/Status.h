#pragma once
#include <windows.h>

#include <string>
#include <utility>

namespace rf {

class Status {
public:
    Status() = default;

    static Status Ok() { return Status{}; }
    static Status Fail(HRESULT hr, std::string msg) { return Status{hr, std::move(msg)}; }
    static Status Fail(std::string msg) { return Status{E_FAIL, std::move(msg)}; }

    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr_); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] HRESULT hr() const noexcept { return hr_; }
    [[nodiscard]] const std::string& message() const noexcept { return msg_; }

    [[nodiscard]] std::string str() const;

private:
    Status(HRESULT hr, std::string msg) : hr_(hr), msg_(std::move(msg)) {}

    HRESULT hr_ = S_OK;
    std::string msg_;
};

}

#define RF_TRY(expr)                             \
    do {                                         \
        ::rf::Status rf_s__ = (expr);            \
        if (!rf_s__.ok()) return rf_s__;         \
    } while (0)

#define RF_HR(expr)                                                    \
    do {                                                               \
        const HRESULT rf_hr__ = (expr);                                \
        if (FAILED(rf_hr__))                                           \
            return ::rf::Status::Fail(rf_hr__, #expr);                 \
    } while (0)
