#pragma once

// === C++ includes ===
#include <cstdint>
#include <source_location>
#include <string_view>

namespace Cue
{
    /// @brief 処理結果の種別です。
    enum class Code : uint16_t
    {
        OK = 0,             // 成功
        InvalidArgument,    // 無効な引数
        NotFound,           // 見つからない
        Unsupported,        // 非対応
        OutOfMemory,        // メモリ不足
        AccessDenied,       // アクセス拒否
        DeviceLost,         // デバイス喪失
        InitializeFailed,   // 初期化失敗
        CreateFailed,       // 作成失敗
        GetFailed,          // 取得失敗
        InvalidState,       // 無効な状態
        InternalError,      // 内部エラー
        UnknownError,       // 不明なエラー
    };

    /// @brief 結果コードを文字列へ変換します。
    /// @param a_code 変換対象の結果コードです。
    /// @return 結果コード名です。
    [[nodiscard]] inline const char* to_string(Code a_code) noexcept
    {
        switch (a_code)
        {
        case Code::OK: return "OK";
        case Code::InvalidArgument: return "InvalidArgument";
        case Code::NotFound: return "NotFound";
        case Code::Unsupported: return "Unsupported";
        case Code::OutOfMemory: return "OutOfMemory";
        case Code::AccessDenied: return "AccessDenied";
        case Code::DeviceLost: return "DeviceLost";
        case Code::InitializeFailed: return "InitializeFailed";
        case Code::CreateFailed: return "CreateFailed";
        case Code::GetFailed: return "GetFailed";
        case Code::InternalError: return "InternalError";
        case Code::UnknownError: return "UnknownError";
        default: return "UnknownCode";
        }
    }

    /// @brief 結果コードが成功かを返します。
    /// @param a_code 判定対象の結果コードです。
    /// @return 成功なら `true` です。
    inline bool success(const Code& a_code) noexcept
    {
        return a_code == Code::OK;
    }

    /// @brief 結果の重大度です。
    enum class Severity : uint8_t
    {
        Info = 0,
        Warning,
        Error,
        Fatal,
    };

    /// @brief 重大度を文字列へ変換します。
    /// @param a_severity 変換対象の重大度です。
    /// @return 重大度名です。
    [[nodiscard]] inline const char* to_string(Severity a_severity) noexcept
    {
        switch (a_severity)
        {
        case Severity::Info: return "Info";
        case Severity::Warning: return "Warning";
        case Severity::Error: return "Error";
        case Severity::Fatal: return "Fatal";
        default: return "UnknownSeverity";
        }
    }

    /// @brief エラー情報を保持する結果構造体です。
    struct Result final
    {
        // 暗黙変換禁止
        explicit Result() = default;

        Code code = Code::OK; // 結果コード
        Severity severity = Severity::Info; // 重大度

        // メッセージ
        // -- 静的文字列前提
        // -- string_view で非所有
        std::string_view message{};

        // ソース位置
        const char* file = "";
        const char* function = "";
        uint32_t line = 0;

        /// @brief 成功結果を返します。
        /// @return 既定の成功結果です。
        static Result ok() noexcept
        {
            return Result{};
        }

        /// @brief 失敗結果を構築します。
        /// @param a_code 結果コードです。
        /// @param a_severity 重大度です。
        /// @param a_message メッセージです。
        /// @param a_location 呼び出し位置です。
        /// @return 構築した失敗結果です。
        static Result fail(
            Code a_code,
            Severity a_severity,
            std::string_view a_message,
            const std::source_location& a_location = std::source_location::current()
        ) noexcept
        {
            // 1) 結果を組み立てる
            Result result{};
            result.code = a_code;
            result.severity = a_severity;
            result.message = a_message;
            result.file = a_location.file_name();
            result.function = a_location.function_name();
            result.line = static_cast<uint32_t>(a_location.line());
            return result;
        }

        /// @brief 成否を bool として返します。
        /// @return `Code::OK` の場合のみ `true` です。
        explicit operator bool() const noexcept
        {
            return code == Code::OK;
        }
    };   

} // namespace Cue
