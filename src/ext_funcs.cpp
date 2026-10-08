/*
 * DuckDB Loadable Extension — Statistical Functions
 *
 * A port of sqlite3-stats (the SQLite3 loadable extension) to DuckDB's C++
 * extension API. The statistical code (calc_* / wf_* / sf_*) is shared with the
 * SQLite version; only the layer that talks to the database engine differs.
 *
 * Basic aggregates:         single-column aggregate functions (Template A)
 * Parameterized aggregates: parameterized aggregate functions (Template B)
 * Two-column aggregates:    two-column aggregate functions (Template C)
 * Window functions:         full-partition window functions (Template D)
 * Complex aggregates:       JSON-returning aggregate functions (Template E)
 * Scalar functions:         scalar functions (Template F)
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <statcpp/statcpp.hpp>

namespace duckdb_stats {

using duckdb::AggregateFunction;
using duckdb::AggregateFunctionSet;
using duckdb::AggregateInputData;
using duckdb::ColumnDataScanState;
using duckdb::DataChunk;
using duckdb::ExpressionState;
using duckdb::ExtensionLoader;
using duckdb::FlatVector;
using duckdb::FunctionNullHandling;
using duckdb::LogicalType;
using duckdb::ScalarFunction;
using duckdb::ScalarFunctionSet;
using duckdb::StringVector;
using duckdb::SubFrames;
using duckdb::UnifiedVectorFormat;
using duckdb::Vector;
using duckdb::VectorType;
using duckdb::WindowPartitionInput;
using duckdb::data_ptr_t;
using duckdb::const_data_ptr_t;
using duckdb::idx_t;

// ===========================================================================
// Exception boundary
//
// statcpp reports invalid arguments by throwing std::invalid_argument. DuckDB
// reports a SQL error by throwing one of its own exception types, so every
// callback runs its body through invoke_guarded(), which converts anything that
// is not already a DuckDB exception into an InvalidInputException. A plain SQL
// expression such as stat_poisson_quantile(1.5, 2.5) thus fails as an ordinary
// SQL error instead of surfacing as an unknown C++ exception.
// ===========================================================================

/**
 * @brief Run a callback body, converting any foreign exception into a SQL error
 * @param body Work to run (a callable taking no arguments)
 */
template <typename Body>
static void invoke_guarded(Body&& body) {
    try {
        std::forward<Body>(body)();
    } catch (const duckdb::Exception&) {
        throw;  // Already a DuckDB error: pass it through unchanged
    } catch (const std::exception& e) {
        throw duckdb::InvalidInputException(e.what());
    } catch (...) {
        throw duckdb::InvalidInputException("DuckDB-stats: unknown error");
    }
}

// ===========================================================================
// Argument and result access
//
// The SQLite version reads its arguments with sqlite3_value_double() and friends
// and writes its result with sqlite3_result_*(). ArgRow and ResultSlot are the
// DuckDB counterparts with the same semantics, so the per-function code reads
// the same in both versions:
//
//   - A NULL argument reads as 0.0 (as sqlite3_value_double() does)
//   - An integer read truncates toward zero (as sqlite3_value_int64() does)
//   - NaN / Inf results and empty text results become SQL NULL
//
// Every argument is registered as DOUBLE, so DuckDB casts integer, decimal and
// float inputs to double before they reach this layer.
// ===========================================================================

/**
 * @brief Arguments of every row of a DataChunk, in unified (flattened) form
 */
class InputColumns {
public:
    /**
     * @param inputs Argument vectors (all of type DOUBLE)
     * @param column_count Number of argument vectors
     * @param row_count Number of rows
     */
    InputColumns(Vector inputs[], idx_t column_count, idx_t row_count) : formats_(column_count) {
        for (idx_t c = 0; c < column_count; ++c) {
            inputs[c].ToUnifiedFormat(row_count, formats_[c]);
        }
    }

    /// @brief Number of argument columns
    idx_t ColumnCount() const { return formats_.size(); }

    /// @brief Whether the argument in column col of row is NULL
    bool IsNull(idx_t row, idx_t col) const {
        const auto& f = formats_[col];
        return !f.validity.RowIsValid(f.sel->get_index(row));
    }

    /// @brief The argument in column col of row as a double (NULL reads as 0.0)
    double Double(idx_t row, idx_t col) const {
        if (IsNull(row, col)) return 0.0;
        const auto& f = formats_[col];
        return UnifiedVectorFormat::GetData<double>(f)[f.sel->get_index(row)];
    }

private:
    std::vector<UnifiedVectorFormat> formats_;  ///< One unified view per argument column
};

/**
 * @brief The arguments of a single row (the counterpart of sqlite3_value** argv)
 */
class ArgRow {
public:
    /**
     * @param columns Argument columns of the chunk
     * @param row Row index within the chunk
     */
    ArgRow(const InputColumns& columns, idx_t row) : columns_(columns), row_(row) {}

    /// @brief Number of arguments (the counterpart of argc)
    int Count() const { return static_cast<int>(columns_.ColumnCount()); }

    /// @brief Whether argument i is NULL
    bool IsNull(int i) const { return columns_.IsNull(row_, static_cast<idx_t>(i)); }

    /// @brief Argument i as a double (NULL reads as 0.0)
    double Double(int i) const { return columns_.Double(row_, static_cast<idx_t>(i)); }

    /// @brief Argument i as an int64, truncated toward zero and saturated at the int64 range
    std::int64_t Int64(int i) const {
        constexpr double kInt64Limit = 9223372036854775807.0;
        const double v = Double(i);
        if (std::isnan(v)) return 0;
        if (v >= kInt64Limit) return std::numeric_limits<std::int64_t>::max();
        if (v <= -kInt64Limit) return std::numeric_limits<std::int64_t>::min();
        return static_cast<std::int64_t>(v);
    }

    /// @brief Argument i as an int (the counterpart of sqlite3_value_int)
    int Int(int i) const {
        const std::int64_t v = Int64(i);
        if (v > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
        if (v < std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
        return static_cast<int>(v);
    }

private:
    const InputColumns& columns_;  ///< Argument columns of the chunk
    idx_t row_;                    ///< Row index within the chunk
};

/**
 * @brief One result cell (the counterpart of sqlite3_context for the result)
 *
 * The cell's SQL type is fixed when the function is registered, so a value of a
 * different C++ type is converted to it: an int64 written to a DOUBLE result is
 * widened, and a double written to a BIGINT result is truncated.
 */
class ResultSlot {
public:
    /**
     * @param result Result vector
     * @param index Row index within the result vector
     */
    ResultSlot(Vector& result, idx_t index) : result_(result), index_(index) {}

    /// @brief Set SQL NULL
    void SetNull() { FlatVector::SetNull(result_, index_, true); }

    /// @brief Set a double; NaN and Inf become NULL
    void SetDouble(double v) {
        if (std::isnan(v) || std::isinf(v)) {
            SetNull();
            return;
        }
        if (result_.GetType().id() == duckdb::LogicalTypeId::BIGINT) {
            FlatVector::GetData<std::int64_t>(result_)[index_] = static_cast<std::int64_t>(v);
        } else {
            FlatVector::GetData<double>(result_)[index_] = v;
        }
    }

    /// @brief Set an int64
    void SetInt64(std::int64_t v) {
        if (result_.GetType().id() == duckdb::LogicalTypeId::DOUBLE) {
            FlatVector::GetData<double>(result_)[index_] = static_cast<double>(v);
        } else {
            FlatVector::GetData<std::int64_t>(result_)[index_] = v;
        }
    }

    /// @brief Set text; an empty string becomes NULL
    void SetText(const std::string& s) {
        if (s.empty()) {
            SetNull();
            return;
        }
        FlatVector::GetData<duckdb::string_t>(result_)[index_] = StringVector::AddString(result_, s);
    }

private:
    Vector& result_;  ///< Result vector
    idx_t index_;     ///< Row index within the result vector
};

// ===========================================================================
// Aggregate engine
//
// DuckDB gives every group a fixed-size state slot. As in the SQLite version
// (which stores a pointer in sqlite3_aggregate_context), the slot holds only a
// pointer to a heap-allocated State, created on the first row and owned by the
// slot: the destructor callback deletes it, so it is released on every path.
//
// Each aggregate is described by a Spec:
//
//   struct Spec {
//       struct State { ... };
//       static void Step(State&, const ArgRow&);         // xStep
//       static void Merge(State& target, const State&);  // combine
//       static void Final(const State&, ResultSlot&);    // xFinal
//   };
//
// Merge must leave its source intact: when an aggregate is evaluated over a
// window frame, DuckDB combines segment-tree nodes that it reuses afterwards.
// ===========================================================================

/**
 * @brief Generic DuckDB aggregate built from a Spec
 * @tparam Spec Aggregate description (see above)
 */
template <class Spec>
class StatAggregate {
public:
    using State = typename Spec::State;

    /// @brief The per-group state slot: a pointer to the heap-allocated State
    struct Slot {
        State* state;  ///< nullptr until the first row arrives
    };

    /**
     * @brief Build a DuckDB aggregate function
     * @param arguments Argument types
     * @param return_type Return type
     */
    static AggregateFunction Make(const duckdb::vector<LogicalType>& arguments, const LogicalType& return_type) {
        AggregateFunction fn(arguments, return_type, StateSize, Initialize, Update, Combine, Finalize,
                             FunctionNullHandling::SPECIAL_HANDLING, SimpleUpdate, nullptr, Destroy);
        return fn;
    }

    static idx_t StateSize(const AggregateFunction&) { return sizeof(Slot); }

    static void Initialize(const AggregateFunction&, data_ptr_t slot) { reinterpret_cast<Slot*>(slot)->state = nullptr; }

    static void Update(Vector inputs[], AggregateInputData&, idx_t input_count, Vector& states, idx_t count) {
        invoke_guarded([&] {
            InputColumns columns(inputs, input_count, count);
            UnifiedVectorFormat sfmt;
            states.ToUnifiedFormat(count, sfmt);
            auto slots = UnifiedVectorFormat::GetData<Slot*>(sfmt);
            for (idx_t i = 0; i < count; ++i) {
                Spec::Step(GetOrCreate(*slots[sfmt.sel->get_index(i)]), ArgRow(columns, i));
            }
        });
    }

    static void SimpleUpdate(Vector inputs[], AggregateInputData&, idx_t input_count, data_ptr_t slot, idx_t count) {
        invoke_guarded([&] {
            InputColumns columns(inputs, input_count, count);
            State& state = GetOrCreate(*reinterpret_cast<Slot*>(slot));
            for (idx_t i = 0; i < count; ++i) {
                Spec::Step(state, ArgRow(columns, i));
            }
        });
    }

    static void Combine(Vector& source, Vector& target, AggregateInputData&, idx_t count) {
        invoke_guarded([&] {
            UnifiedVectorFormat sfmt;
            source.ToUnifiedFormat(count, sfmt);
            auto sources = UnifiedVectorFormat::GetData<Slot*>(sfmt);
            auto targets = FlatVector::GetData<Slot*>(target);
            for (idx_t i = 0; i < count; ++i) {
                const Slot& src = *sources[sfmt.sel->get_index(i)];
                if (!src.state) continue;
                Spec::Merge(GetOrCreate(*targets[i]), *src.state);
            }
        });
    }

    static void Finalize(Vector& states, AggregateInputData&, Vector& result, idx_t count, idx_t offset) {
        invoke_guarded([&] {
            if (states.GetVectorType() == VectorType::CONSTANT_VECTOR) {
                // Ungrouped aggregate: a single state
                result.SetVectorType(VectorType::CONSTANT_VECTOR);
                auto slot = duckdb::ConstantVector::GetData<Slot*>(states)[0];
                FinalizeOne(*slot, result, 0);
                return;
            }
            result.SetVectorType(VectorType::FLAT_VECTOR);
            auto slots = FlatVector::GetData<Slot*>(states);
            for (idx_t i = 0; i < count; ++i) {
                FinalizeOne(*slots[i], result, i + offset);
            }
        });
    }

    static void Destroy(Vector& states, AggregateInputData&, idx_t count) {
        UnifiedVectorFormat sfmt;
        states.ToUnifiedFormat(count, sfmt);
        auto slots = UnifiedVectorFormat::GetData<Slot*>(sfmt);
        for (idx_t i = 0; i < count; ++i) {
            Slot& slot = *slots[sfmt.sel->get_index(i)];
            delete slot.state;
            slot.state = nullptr;
        }
    }

    /// @brief Return the slot's State, creating it on first use
    static State& GetOrCreate(Slot& slot) {
        if (!slot.state) slot.state = new State();
        return *slot.state;
    }

private:
    static void FinalizeOne(const Slot& slot, Vector& result, idx_t index) {
        ResultSlot out(result, index);
        if (!slot.state) {
            out.SetNull();  // No rows at all
            return;
        }
        Spec::Final(*slot.state, out);
    }
};

// ===========================================================================
// Template A — SingleColumnAggregate
//
// Collects the non-NULL values of one column and computes a statistic.
// Func signature: double(const std::vector<double>&)
// ===========================================================================

struct SingleColumnState {
    std::vector<double> values;
};

template <double (*Func)(const std::vector<double>&)>
struct SingleColumnAggregate {
    using State = SingleColumnState;

    static void Step(State& s, const ArgRow& argv) {
        if (argv.IsNull(0)) return;
        s.values.push_back(argv.Double(0));
    }

    static void Merge(State& t, const State& s) { t.values.insert(t.values.end(), s.values.begin(), s.values.end()); }

    static void Final(const State& s, ResultSlot& out) {
        if (s.values.empty()) {
            out.SetNull();
            return;
        }
        out.SetDouble(Func(s.values));
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        AggregateFunctionSet set(name);
        set.AddFunction(StatAggregate<SingleColumnAggregate>::Make({LogicalType::DOUBLE}, LogicalType::DOUBLE));
        loader.RegisterFunction(set);
    }
};

// ===========================================================================
// Wrapper functions: adapt statcpp API to (const vector<double>&) -> double
//
// statcpp functions use iterator pairs. Some (median, iqr) require sorted
// input, so we sort a local copy where needed.
// ===========================================================================

// --- Basic statistics ---

static double calc_mean(const std::vector<double>& v) {
    return statcpp::mean(v.begin(), v.end());
}

static double calc_median(const std::vector<double>& v) {
    std::vector<double> sorted(v);
    std::sort(sorted.begin(), sorted.end());
    return statcpp::median(sorted.begin(), sorted.end());
}

static double calc_mode(const std::vector<double>& v) {
    return statcpp::mode(v.begin(), v.end());
}

static double calc_geometric_mean(const std::vector<double>& v) {
    return statcpp::geometric_mean(v.begin(), v.end());
}

static double calc_harmonic_mean(const std::vector<double>& v) {
    return statcpp::harmonic_mean(v.begin(), v.end());
}

// --- Dispersion / spread ---

static double calc_range(const std::vector<double>& v) {
    return statcpp::range(v.begin(), v.end());
}

static double calc_var(const std::vector<double>& v) {
    // var with ddof=0 (population variance) — same as population_variance
    // Note: stat_var uses ddof=0 by default as per statcpp convention
    return statcpp::var(v.begin(), v.end(), static_cast<std::size_t>(0));
}

static double calc_population_variance(const std::vector<double>& v) {
    return statcpp::population_variance(v.begin(), v.end());
}

static double calc_sample_variance(const std::vector<double>& v) {
    if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::sample_variance(v.begin(), v.end());
}

static double calc_stdev(const std::vector<double>& v) {
    return statcpp::stdev(v.begin(), v.end(), static_cast<std::size_t>(0));
}

static double calc_population_stddev(const std::vector<double>& v) {
    return statcpp::population_stddev(v.begin(), v.end());
}

static double calc_sample_stddev(const std::vector<double>& v) {
    if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::sample_stddev(v.begin(), v.end());
}

static double calc_cv(const std::vector<double>& v) {
    return statcpp::coefficient_of_variation(v.begin(), v.end());
}

static double calc_iqr(const std::vector<double>& v) {
    std::vector<double> sorted(v);
    std::sort(sorted.begin(), sorted.end());
    return statcpp::iqr(sorted.begin(), sorted.end());
}

static double calc_mad_mean(const std::vector<double>& v) {
    return statcpp::mean_absolute_deviation(v.begin(), v.end());
}

static double calc_geometric_stddev(const std::vector<double>& v) {
    return statcpp::geometric_stddev(v.begin(), v.end());
}

// --- Shape of distribution ---

static double calc_population_skewness(const std::vector<double>& v) {
    if (v.size() < 3) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::population_skewness(v.begin(), v.end());
}

static double calc_skewness(const std::vector<double>& v) {
    if (v.size() < 3) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::sample_skewness(v.begin(), v.end());
}

static double calc_population_kurtosis(const std::vector<double>& v) {
    if (v.size() < 4) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::population_kurtosis(v.begin(), v.end());
}

static double calc_kurtosis(const std::vector<double>& v) {
    if (v.size() < 4) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::sample_kurtosis(v.begin(), v.end());
}

// --- Estimation ---

static double calc_se(const std::vector<double>& v) {
    if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::standard_error(v.begin(), v.end());
}

// --- Robust statistics ---

static double calc_mad(const std::vector<double>& v) {
    return statcpp::mad(v.begin(), v.end());
}

static double calc_mad_scaled(const std::vector<double>& v) {
    return statcpp::mad_scaled(v.begin(), v.end());
}

static double calc_hodges_lehmann(const std::vector<double>& v) {
    return statcpp::hodges_lehmann(v.begin(), v.end());
}

// ===========================================================================
// Template B — SingleColumnParamAggregate
//
// Like Template A but with additional parameters captured from the extra SQL
// arguments of the first non-NULL row.
// Func signature: double/std::string (const vector<double>&, const array<double,N>&)
// ===========================================================================

template <std::size_t NParams>
struct SingleColumnParamState {
    std::vector<double> values;
    std::array<double, NParams> params{};
    bool params_set = false;
    bool params_null = false;  ///< A parameter was NULL: the result is NULL (SQL convention)
};

/**
 * @brief Shared Step/Merge for the single-column parameterized aggregates
 * @tparam NParams Number of parameters following the value column
 */
template <std::size_t NParams>
struct SingleColumnParamCollect {
    using State = SingleColumnParamState<NParams>;

    static void Step(State& s, const ArgRow& argv) {
        if (argv.IsNull(0)) return;
        s.values.push_back(argv.Double(0));
        if (!s.params_set) {
            for (std::size_t i = 0; i < NParams; ++i) {
                const int arg = static_cast<int>(i + 1);
                if (arg >= argv.Count()) continue;  // omitted: keep the default
                if (argv.IsNull(arg)) {
                    s.params_null = true;
                } else {
                    s.params[i] = argv.Double(arg);
                }
            }
            s.params_set = true;
        }
    }

    static void Merge(State& t, const State& s) {
        t.values.insert(t.values.end(), s.values.begin(), s.values.end());
        if (!t.params_set && s.params_set) {
            t.params = s.params;
            t.params_set = true;
            t.params_null = s.params_null;
        }
    }
};

// --- B1: returns double ---

template <std::size_t NParams, double (*Func)(const std::vector<double>&, const std::array<double, NParams>&)>
struct SingleColumnParamAggregate : SingleColumnParamCollect<NParams> {
    using State = SingleColumnParamState<NParams>;

    static void Final(const State& s, ResultSlot& out) {
        if (s.values.empty() || s.params_null) {
            out.SetNull();
            return;
        }
        out.SetDouble(Func(s.values, s.params));
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        duckdb::vector<LogicalType> args(1 + NParams, LogicalType::DOUBLE);
        AggregateFunctionSet set(name);
        set.AddFunction(StatAggregate<SingleColumnParamAggregate>::Make(args, LogicalType::DOUBLE));
        loader.RegisterFunction(set);
    }
};

// --- B2: returns JSON text ---

template <std::size_t NParams, std::string (*Func)(const std::vector<double>&, const std::array<double, NParams>&)>
struct SingleColumnParamAggregateText : SingleColumnParamCollect<NParams> {
    using State = SingleColumnParamState<NParams>;

    static void Final(const State& s, ResultSlot& out) {
        if (s.values.empty() || s.params_null) {
            out.SetNull();
            return;
        }
        out.SetText(Func(s.values, s.params));
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        duckdb::vector<LogicalType> args(1 + NParams, LogicalType::DOUBLE);
        AggregateFunctionSet set(name);
        set.AddFunction(StatAggregate<SingleColumnParamAggregateText>::Make(args, LogicalType::VARCHAR));
        loader.RegisterFunction(set);
    }
};

// ===========================================================================
// JSON formatting helpers (minimal, no external dependency)
// ===========================================================================

static std::string json_double(double v) {
    if (std::isnan(v) || std::isinf(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
}

static std::string json_test_result(const statcpp::test_result& r) {
    return "{\"statistic\":" + json_double(r.statistic)
         + ",\"p_value\":" + json_double(r.p_value)
         + ",\"df\":" + json_double(r.df) + "}";
}

static std::string json_ci(const statcpp::confidence_interval& ci) {
    return "{\"lower\":" + json_double(ci.lower)
         + ",\"upper\":" + json_double(ci.upper)
         + ",\"point_estimate\":" + json_double(ci.point_estimate)
         + ",\"confidence_level\":" + json_double(ci.confidence_level) + "}";
}

static std::string json_bootstrap(const statcpp::bootstrap_result& br) {
    return "{\"estimate\":" + json_double(br.estimate)
         + ",\"standard_error\":" + json_double(br.standard_error)
         + ",\"ci_lower\":" + json_double(br.ci_lower)
         + ",\"ci_upper\":" + json_double(br.ci_upper)
         + ",\"bias\":" + json_double(br.bias) + "}";
}

// ===========================================================================
// Parameterized aggregate wrapper functions
// ===========================================================================

// --- Basic statistics (parameterized) ---

static double calc_trimmed_mean(const std::vector<double>& v,
                                const std::array<double, 1>& p) {
    std::vector<double> sorted(v);
    std::sort(sorted.begin(), sorted.end());
    return statcpp::trimmed_mean(sorted.begin(), sorted.end(), p[0]);
}

// --- Order statistics (parameterized) ---

static std::string calc_quartile(const std::vector<double>& v,
                                 const std::array<double, 0>&) {
    std::vector<double> sorted(v);
    std::sort(sorted.begin(), sorted.end());
    auto qr = statcpp::quartiles(sorted.begin(), sorted.end());
    return "{\"q1\":" + json_double(qr.q1)
         + ",\"q2\":" + json_double(qr.q2)
         + ",\"q3\":" + json_double(qr.q3) + "}";
}

static double calc_percentile(const std::vector<double>& v,
                               const std::array<double, 1>& p) {
    std::vector<double> sorted(v);
    std::sort(sorted.begin(), sorted.end());
    return statcpp::percentile(sorted.begin(), sorted.end(), p[0]);
}

// --- Parametric tests ---

static std::string calc_z_test(const std::vector<double>& v,
                                const std::array<double, 2>& p) {
    auto r = statcpp::z_test(v.begin(), v.end(), p[0], p[1]);
    return json_test_result(r);
}

static std::string calc_t_test(const std::vector<double>& v,
                                const std::array<double, 1>& p) {
    if (v.size() < 2) return "";
    auto r = statcpp::t_test(v.begin(), v.end(), p[0]);
    return json_test_result(r);
}

static std::string calc_chisq_gof_uniform(const std::vector<double>& v,
                                           const std::array<double, 0>&) {
    if (v.size() < 2) return "";
    auto r = statcpp::chisq_test_gof_uniform(v.begin(), v.end());
    return json_test_result(r);
}

// --- Nonparametric tests ---

static std::string calc_shapiro_wilk(const std::vector<double>& v,
                                      const std::array<double, 0>&) {
    if (v.size() < 3) return "";
    auto r = statcpp::shapiro_wilk_test(v.begin(), v.end());
    return json_test_result(r);
}

static std::string calc_ks_test(const std::vector<double>& v,
                                 const std::array<double, 0>&) {
    if (v.size() < 2) return "";
    auto r = statcpp::lilliefors_test(v.begin(), v.end());
    return json_test_result(r);
}

static std::string calc_wilcoxon(const std::vector<double>& v,
                                  const std::array<double, 1>& p) {
    if (v.size() < 2) return "";
    auto r = statcpp::wilcoxon_signed_rank_test(v.begin(), v.end(), p[0]);
    return json_test_result(r);
}

// --- Estimation ---

static std::string calc_ci_mean(const std::vector<double>& v,
                                 const std::array<double, 1>& p) {
    if (v.size() < 2) return "";
    auto ci = statcpp::ci_mean(v.begin(), v.end(), p[0]);
    return json_ci(ci);
}

static std::string calc_ci_mean_z(const std::vector<double>& v,
                                   const std::array<double, 2>& p) {
    if (v.size() < 2) return "";
    auto ci = statcpp::ci_mean_z(v.begin(), v.end(), p[0], p[1]);
    return json_ci(ci);
}

static std::string calc_ci_var(const std::vector<double>& v,
                                const std::array<double, 1>& p) {
    if (v.size() < 2) return "";
    auto ci = statcpp::ci_variance(v.begin(), v.end(), p[0]);
    return json_ci(ci);
}

static double calc_moe_mean(const std::vector<double>& v,
                             const std::array<double, 1>& p) {
    if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::margin_of_error_mean(v.begin(), v.end(), p[0]);
}

// --- Effect size ---

static double calc_cohens_d(const std::vector<double>& v,
                             const std::array<double, 1>& p) {
    if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::cohens_d(v.begin(), v.end(), p[0]);
}

static double calc_hedges_g(const std::vector<double>& v,
                             const std::array<double, 1>& p) {
    if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::hedges_g(v.begin(), v.end(), p[0]);
}

// --- Time series ---

static double calc_acf_lag(const std::vector<double>& v,
                            const std::array<double, 1>& p) {
    auto lag = static_cast<std::size_t>(p[0]);
    if (lag >= v.size()) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::autocorrelation(v.begin(), v.end(), lag);
}

// --- Robust statistics (parameterized) ---

static double calc_biweight_midvar(const std::vector<double>& v,
                                    const std::array<double, 1>& p) {
    return statcpp::biweight_midvariance(v.begin(), v.end(), p[0]);
}

// --- Resampling ---

static std::string calc_bootstrap_mean(const std::vector<double>& v,
                                        const std::array<double, 1>& p) {
    auto n = static_cast<std::size_t>(p[0]);
    if (n < 2) n = 1000;
    auto r = statcpp::bootstrap_mean(v.begin(), v.end(), n);
    return json_bootstrap(r);
}

static std::string calc_bootstrap_median(const std::vector<double>& v,
                                          const std::array<double, 1>& p) {
    auto n = static_cast<std::size_t>(p[0]);
    if (n < 2) n = 1000;
    auto r = statcpp::bootstrap_median(v.begin(), v.end(), n);
    return json_bootstrap(r);
}

static std::string calc_bootstrap_stddev(const std::vector<double>& v,
                                          const std::array<double, 1>& p) {
    if (v.size() < 2) return "";
    auto n = static_cast<std::size_t>(p[0]);
    if (n < 2) n = 1000;
    auto r = statcpp::bootstrap_stddev(v.begin(), v.end(), n);
    return json_bootstrap(r);
}

// ===========================================================================
// Template C — TwoColumnAggregate
//
// Collects (x, y) pairs. If either x or y is NULL for a row, that row is skipped.
// Func signature: double(const std::vector<double>&, const std::vector<double>&)
// ===========================================================================

struct TwoColumnState {
    std::vector<double> xs;
    std::vector<double> ys;
};

/// @brief Shared Step/Merge for the two-column aggregates without parameters
struct TwoColumnCollect {
    using State = TwoColumnState;

    static void Step(State& s, const ArgRow& argv) {
        if (argv.IsNull(0) || argv.IsNull(1)) return;
        s.xs.push_back(argv.Double(0));
        s.ys.push_back(argv.Double(1));
    }

    static void Merge(State& t, const State& s) {
        t.xs.insert(t.xs.end(), s.xs.begin(), s.xs.end());
        t.ys.insert(t.ys.end(), s.ys.begin(), s.ys.end());
    }
};

template <double (*Func)(const std::vector<double>&, const std::vector<double>&)>
struct TwoColumnAggregate : TwoColumnCollect {
    static void Final(const State& s, ResultSlot& out) {
        if (s.xs.empty()) {
            out.SetNull();
            return;
        }
        out.SetDouble(Func(s.xs, s.ys));
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        AggregateFunctionSet set(name);
        set.AddFunction(StatAggregate<TwoColumnAggregate>::Make({LogicalType::DOUBLE, LogicalType::DOUBLE},
                                                                LogicalType::DOUBLE));
        loader.RegisterFunction(set);
    }
};

// --- C variant: returns JSON text ---

template <std::string (*Func)(const std::vector<double>&, const std::vector<double>&)>
struct TwoColumnAggregateText : TwoColumnCollect {
    static void Final(const State& s, ResultSlot& out) {
        if (s.xs.empty()) {
            out.SetNull();
            return;
        }
        out.SetText(Func(s.xs, s.ys));
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        AggregateFunctionSet set(name);
        set.AddFunction(StatAggregate<TwoColumnAggregateText>::Make({LogicalType::DOUBLE, LogicalType::DOUBLE},
                                                                    LogicalType::VARCHAR));
        loader.RegisterFunction(set);
    }
};

// --- C variant: with extra parameter(s) ---

template <std::size_t NParams>
struct TwoColumnParamState {
    std::vector<double> xs;
    std::vector<double> ys;
    std::array<double, NParams> params{};
    bool params_set = false;
    bool params_null = false;  ///< A parameter was NULL: the result is NULL (SQL convention)
};

/**
 * @brief Shared Step/Merge for the two-column parameterized aggregates
 * @tparam NParams Number of parameters following the two value columns
 */
template <std::size_t NParams>
struct TwoColumnParamCollect {
    using State = TwoColumnParamState<NParams>;

    static void Step(State& s, const ArgRow& argv) {
        if (argv.IsNull(0) || argv.IsNull(1)) return;
        s.xs.push_back(argv.Double(0));
        s.ys.push_back(argv.Double(1));
        if (!s.params_set) {
            for (std::size_t i = 0; i < NParams; ++i) {
                const int arg = static_cast<int>(i + 2);
                if (arg >= argv.Count()) continue;  // omitted: keep the default
                if (argv.IsNull(arg)) {
                    s.params_null = true;
                } else {
                    s.params[i] = argv.Double(arg);
                }
            }
            s.params_set = true;
        }
    }

    static void Merge(State& t, const State& s) {
        t.xs.insert(t.xs.end(), s.xs.begin(), s.xs.end());
        t.ys.insert(t.ys.end(), s.ys.begin(), s.ys.end());
        if (!t.params_set && s.params_set) {
            t.params = s.params;
            t.params_set = true;
            t.params_null = s.params_null;
        }
    }
};

template <std::size_t NParams,
          double (*Func)(const std::vector<double>&,
                         const std::vector<double>&,
                         const std::array<double, NParams>&)>
struct TwoColumnParamAggregate : TwoColumnParamCollect<NParams> {
    using State = TwoColumnParamState<NParams>;

    static void Final(const State& s, ResultSlot& out) {
        if (s.xs.empty() || s.params_null) {
            out.SetNull();
            return;
        }
        out.SetDouble(Func(s.xs, s.ys, s.params));
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        duckdb::vector<LogicalType> args(2 + NParams, LogicalType::DOUBLE);
        AggregateFunctionSet set(name);
        set.AddFunction(StatAggregate<TwoColumnParamAggregate>::Make(args, LogicalType::DOUBLE));
        loader.RegisterFunction(set);
    }
};

// --- C variant: with extra parameter(s), returns JSON text ---
//
// TwoColumnAggregateText with parameters added. Used where two columns plus a
// parameter produce a structured result: post-hoc tests over a group column
// (which take alpha), stratified sampling (which takes a ratio), and the like.
//
// Every arity from 2 to 2 + NParams is registered: an omitted trailing parameter
// keeps its default (0.0), which the wrapper function resolves.

template <std::size_t NParams,
          std::string (*Func)(const std::vector<double>&,
                              const std::vector<double>&,
                              const std::array<double, NParams>&)>
struct TwoColumnParamAggregateText : TwoColumnParamCollect<NParams> {
    using State = TwoColumnParamState<NParams>;

    static void Final(const State& s, ResultSlot& out) {
        if (s.xs.empty() || s.params_null) {
            out.SetNull();
            return;
        }
        out.SetText(Func(s.xs, s.ys, s.params));
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        AggregateFunctionSet set(name);
        for (std::size_t n = 0; n <= NParams; ++n) {
            duckdb::vector<LogicalType> args(2 + n, LogicalType::DOUBLE);
            set.AddFunction(StatAggregate<TwoColumnParamAggregateText>::Make(args, LogicalType::VARCHAR));
        }
        loader.RegisterFunction(set);
    }
};

// ===========================================================================
// Template D — Window Functions
//
// D1: FullScanWindowFunction — computes a result vector over the whole
//     partition once, then returns one element per row.
//
// DuckDB's custom window aggregate API is used: window_init receives the whole
// partition and computes the result vector once; the window callback then
// returns the element for the row being evaluated.
//
// That callback is not told the row's position in the partition, only its frame
// bounds. The row is therefore identified as the LAST ROW OF ITS FRAME, which
// holds for the default frame of OVER (ORDER BY ...) and for
// ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW.
//
// OVER () and full-partition frames cannot work: DuckDB evaluates those with its
// constant aggregator, which runs Finalize once and copies the value to every
// row. Finalize, which also serves a plain aggregate call, therefore raises an
// error explaining the required OVER clause.
// ===========================================================================

struct WindowState {
    std::vector<double> values;
    std::vector<bool> nulls;
    std::vector<double> results;
    int param = 0;
    double dparam = 0.0;
    bool param_set = false;
    bool param_null = false;  ///< The parameter was NULL: every row is NULL (as lag(v, NULL))
};

// Func: (const vector<double>& values, const vector<bool>& nulls, int param) -> vector<double>
template <std::vector<double> (*Func)(const std::vector<double>&,
                                       const std::vector<bool>&,
                                       int)>
struct FullScanWindowFunction {
    using State = WindowState;
    using Engine = StatAggregate<FullScanWindowFunction>;

    static void Step(State& s, const ArgRow& argv) {
        const bool is_null = argv.IsNull(0);
        s.nulls.push_back(is_null);
        s.values.push_back(is_null ? std::numeric_limits<double>::quiet_NaN() : argv.Double(0));
        if (!s.param_set && argv.Count() > 1) {
            if (argv.IsNull(1)) {
                s.param_null = true;
            } else {
                s.param = argv.Int(1);
                s.dparam = argv.Double(1);
            }
            s.param_set = true;
        }
    }

    static void Merge(State& t, const State& s) {
        t.values.insert(t.values.end(), s.values.begin(), s.values.end());
        t.nulls.insert(t.nulls.end(), s.nulls.begin(), s.nulls.end());
        if (!t.param_set && s.param_set) {
            t.param = s.param;
            t.dparam = s.dparam;
            t.param_set = true;
            t.param_null = s.param_null;
        }
    }

    static void Final(const State&, ResultSlot&) {
        throw duckdb::InvalidInputException(
            "this is a window function: call it with an ORDER BY in the OVER clause, "
            "e.g. OVER (ORDER BY id). OVER () and full-partition frames are not supported");
    }

    /// @brief window_init: compute the result vector over the whole partition
    static void WindowInit(AggregateInputData&, const WindowPartitionInput& partition, data_ptr_t g_state) {
        invoke_guarded([&] {
            State& s = Engine::GetOrCreate(*reinterpret_cast<typename Engine::Slot*>(g_state));
            ColumnDataScanState scan;
            DataChunk chunk;
            partition.inputs->InitializeScan(scan, partition.column_ids);
            partition.inputs->InitializeScanChunk(scan, chunk);
            while (partition.inputs->Scan(scan, chunk)) {
                InputColumns columns(chunk.data.data(), chunk.ColumnCount(), chunk.size());
                for (idx_t i = 0; i < chunk.size(); ++i) {
                    Step(s, ArgRow(columns, i));
                }
            }
            if (s.param_null) {
                s.results.assign(s.values.size(), std::numeric_limits<double>::quiet_NaN());
            } else {
                s.results = Func(s.values, s.nulls, s.param);
            }
        });
    }

    /// @brief window: return the element for the row being evaluated (the last row of its frame)
    static void Window(AggregateInputData&, const WindowPartitionInput&, const_data_ptr_t g_state, data_ptr_t,
                       const SubFrames& frames, Vector& result, idx_t rid) {
        invoke_guarded([&] {
            const auto* slot = reinterpret_cast<const typename Engine::Slot*>(g_state);
            ResultSlot out(result, rid);
            const idx_t frame_end = frames.back().end;
            if (!slot->state || frame_end == 0 || frame_end > slot->state->results.size()) {
                out.SetNull();
                return;
            }
            out.SetDouble(slot->state->results[frame_end - 1]);
        });
    }

    static AggregateFunction Make(idx_t argc) {
        duckdb::vector<LogicalType> args(argc, LogicalType::DOUBLE);
        AggregateFunction fn = Engine::Make(args, LogicalType::DOUBLE);
        fn.window = Window;
        fn.SetWindowInitCallback(WindowInit);
        return fn;
    }

    // argc=1: no parameter (e.g. stat_rank, stat_fillna_mean)
    static void register_func_1(ExtensionLoader& loader, const char* name) {
        AggregateFunctionSet set(name);
        set.AddFunction(Make(1));
        loader.RegisterFunction(set);
    }

    // argc=2: with parameter (e.g. stat_rolling_mean(col, window_size))
    static void register_func_2(ExtensionLoader& loader, const char* name) {
        AggregateFunctionSet set(name);
        set.AddFunction(Make(2));
        loader.RegisterFunction(set);
    }
};

// ===========================================================================
// Two-column aggregate wrapper functions
// ===========================================================================

// --- Correlation / Covariance ---

static double calc_population_covariance(const std::vector<double>& x,
                                          const std::vector<double>& y) {
    if (x.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::population_covariance(x.begin(), x.end(), y.begin(), y.end());
}

static double calc_covariance(const std::vector<double>& x,
                               const std::vector<double>& y) {
    if (x.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::covariance(x.begin(), x.end(), y.begin(), y.end());
}

static double calc_pearson_r(const std::vector<double>& x,
                              const std::vector<double>& y) {
    if (x.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::pearson_correlation(x.begin(), x.end(), y.begin(), y.end());
}

static double calc_spearman_r(const std::vector<double>& x,
                               const std::vector<double>& y) {
    if (x.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::spearman_correlation(x.begin(), x.end(), y.begin(), y.end());
}

static double calc_kendall_tau(const std::vector<double>& x,
                                const std::vector<double>& y) {
    if (x.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::kendall_tau(x.begin(), x.end(), y.begin(), y.end());
}

static double calc_weighted_covariance(const std::vector<double>& values,
                                        const std::vector<double>& weights) {
    if (values.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    // weighted_covariance(val_first, val_last, val2_first, val2_last, weight_first)
    // Here col_x = col_y = values, weights = weights (self-covariance = weighted variance)
    // Actually, weighted_covariance needs two data columns and weights.
    // In our 2-column interface, x=values, y=weights is ambiguous.
    // We treat this as: covariance of x with itself, weighted by y — i.e., weighted variance.
    // But the design says stat_weighted_covariance(col_x, col_y) computes covariance
    // with values=col_x, weights=col_y... Let's follow the statcpp API:
    // weighted_covariance(first1, last1, first2, last2, weight_first) needs 3 series.
    // With 2 columns, we compute weighted self-covariance: cov(x, x, w=y)
    // Actually, re-reading the doc: stat_weighted_covariance(values, weights)
    // computes the weighted covariance. This needs 3 columns conceptually,
    // but with 2 columns we treat it as weighted variance of x using weights y.
    return statcpp::weighted_covariance(values.begin(), values.end(),
                                         values.begin(), values.end(),
                                         weights.begin());
}

// --- Weighted statistics ---

static double calc_weighted_mean(const std::vector<double>& values,
                                  const std::vector<double>& weights) {
    return statcpp::weighted_mean(values.begin(), values.end(), weights.begin(), weights.end());
}

static double calc_weighted_harmonic_mean(const std::vector<double>& values,
                                           const std::vector<double>& weights) {
    return statcpp::weighted_harmonic_mean(values.begin(), values.end(), weights.begin(), weights.end());
}

static double calc_weighted_variance(const std::vector<double>& values,
                                      const std::vector<double>& weights) {
    return statcpp::weighted_variance(values.begin(), values.end(), weights.begin(), weights.end());
}

static double calc_weighted_stddev(const std::vector<double>& values,
                                    const std::vector<double>& weights) {
    return statcpp::weighted_stddev(values.begin(), values.end(), weights.begin(), weights.end());
}

static double calc_weighted_median(const std::vector<double>& values,
                                    const std::vector<double>& weights) {
    return statcpp::weighted_median(values.begin(), values.end(), weights.begin(), weights.end());
}

static double calc_weighted_percentile(const std::vector<double>& values,
                                        const std::vector<double>& weights,
                                        const std::array<double, 1>& p) {
    return statcpp::weighted_percentile(values.begin(), values.end(),
                                         weights.begin(), weights.end(), p[0]);
}

// --- Linear regression ---

static std::string json_regression(const statcpp::simple_regression_result& r) {
    return "{\"intercept\":" + json_double(r.intercept)
         + ",\"slope\":" + json_double(r.slope)
         + ",\"r_squared\":" + json_double(r.r_squared)
         + ",\"adj_r_squared\":" + json_double(r.adj_r_squared)
         + ",\"intercept_se\":" + json_double(r.intercept_se)
         + ",\"slope_se\":" + json_double(r.slope_se)
         + ",\"intercept_t\":" + json_double(r.intercept_t)
         + ",\"slope_t\":" + json_double(r.slope_t)
         + ",\"intercept_p\":" + json_double(r.intercept_p)
         + ",\"slope_p\":" + json_double(r.slope_p)
         + ",\"f_statistic\":" + json_double(r.f_statistic)
         + ",\"f_p_value\":" + json_double(r.f_p_value)
         + ",\"residual_se\":" + json_double(r.residual_se) + "}";
}

static std::string calc_simple_regression(const std::vector<double>& x,
                                           const std::vector<double>& y) {
    if (x.size() < 3) return "";
    auto r = statcpp::simple_linear_regression(x.begin(), x.end(),
                                                y.begin(), y.end());
    return json_regression(r);
}

static double calc_r_squared(const std::vector<double>& actual,
                              const std::vector<double>& predicted) {
    if (actual.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::r_squared(actual.begin(), actual.end(),
                               predicted.begin(), predicted.end());
}

static double calc_adjusted_r_squared(const std::vector<double>& actual,
                                       const std::vector<double>& predicted) {
    if (actual.size() < 3) return std::numeric_limits<double>::quiet_NaN();
    // num_predictors = 1 for simple regression (2-column case)
    return statcpp::adjusted_r_squared(actual.begin(), actual.end(),
                                        predicted.begin(), predicted.end(), 1);
}

// --- Parametric tests (two-column) ---

static std::string calc_t_test_paired(const std::vector<double>& x,
                                       const std::vector<double>& y) {
    if (x.size() < 2) return "";
    auto r = statcpp::t_test_paired(x.begin(), x.end(), y.begin(), y.end());
    return json_test_result(r);
}

static std::string calc_chisq_gof(const std::vector<double>& observed,
                                   const std::vector<double>& expected) {
    if (observed.size() < 2) return "";
    auto r = statcpp::chisq_test_gof(observed.begin(), observed.end(),
                                      expected.begin(), expected.end());
    return json_test_result(r);
}

// --- Error metrics ---

static double calc_mae(const std::vector<double>& actual,
                        const std::vector<double>& predicted) {
    return statcpp::mae(actual.begin(), actual.end(), predicted.begin());
}

static double calc_mse(const std::vector<double>& actual,
                        const std::vector<double>& predicted) {
    return statcpp::mse(actual.begin(), actual.end(), predicted.begin());
}

static double calc_rmse(const std::vector<double>& actual,
                         const std::vector<double>& predicted) {
    return statcpp::rmse(actual.begin(), actual.end(), predicted.begin());
}

static double calc_mape(const std::vector<double>& actual,
                         const std::vector<double>& predicted) {
    return statcpp::mape(actual.begin(), actual.end(), predicted.begin());
}

// --- Distance metrics ---

static double calc_euclidean_dist(const std::vector<double>& a,
                                   const std::vector<double>& b) {
    return statcpp::euclidean_distance(a.begin(), a.end(), b.begin(), b.end());
}

static double calc_manhattan_dist(const std::vector<double>& a,
                                   const std::vector<double>& b) {
    return statcpp::manhattan_distance(a.begin(), a.end(), b.begin(), b.end());
}

static double calc_cosine_sim(const std::vector<double>& a,
                               const std::vector<double>& b) {
    return statcpp::cosine_similarity(a.begin(), a.end(), b.begin(), b.end());
}

static double calc_cosine_dist(const std::vector<double>& a,
                                const std::vector<double>& b) {
    return statcpp::cosine_distance(a.begin(), a.end(), b.begin(), b.end());
}

static double calc_minkowski_dist(const std::vector<double>& a,
                                   const std::vector<double>& b,
                                   const std::array<double, 1>& p) {
    return statcpp::minkowski_distance(a.begin(), a.end(),
                                        b.begin(), b.end(), p[0]);
}

static double calc_chebyshev_dist(const std::vector<double>& a,
                                   const std::vector<double>& b) {
    return statcpp::chebyshev_distance(a.begin(), a.end(), b.begin(), b.end());
}

// ===========================================================================
// Window function wrappers
// ===========================================================================

// Helper: extract non-NaN values for statcpp functions that don't handle NaN
static std::vector<double> extract_valid(const std::vector<double>& values,
                                          const std::vector<bool>& nulls) {
    std::vector<double> valid;
    valid.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) valid.push_back(values[i]);
    }
    return valid;
}

// --- Rolling functions ---

/**
 * @brief Place a rolling result on the last row of each window
 *
 * statcpp's rolling functions return one value per complete window (n - w + 1
 * values). Row i receives the statistic of rows i-w+1 .. i, the trailing window
 * that pandas rolling() and SQL's "ROWS w-1 PRECEDING" use; the first w-1 rows
 * and every row whose window contains a NULL become NaN (SQL NULL).
 *
 * The NULL rule is enforced here rather than left to statcpp: rolling_mean / std /
 * sum already return NaN for such a window, but rolling_min / max use
 * std::min_element / std::max_element, which skip a NaN unless it is the window's
 * first element.
 *
 * The SQLite version returned the vector unshifted, which gave row i the window
 * starting at i (rows i .. i+w-1) and left the last w-1 rows NULL.
 *
 * @param windowed One value per complete window, in order
 * @param window Window size
 * @param nulls NULL flag of every input row
 * @return One value per input row
 */
static std::vector<double> align_trailing(const std::vector<double>& windowed, std::size_t window,
                                          const std::vector<bool>& nulls) {
    std::vector<double> out(nulls.size(), std::numeric_limits<double>::quiet_NaN());
    std::size_t nulls_in_window = 0;  // NULL rows among i-w+1 .. i
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (nulls[i]) ++nulls_in_window;
        if (i >= window && nulls[i - window]) --nulls_in_window;
        if (i + 1 < window || nulls_in_window > 0) continue;
        const std::size_t k = i + 1 - window;  // index of the window ending at row i
        if (k < windowed.size()) out[i] = windowed[k];
    }
    return out;
}

static std::vector<double> wf_rolling_mean(const std::vector<double>& values,
                                            const std::vector<bool>& nulls,
                                            int window) {
    if (window <= 0) window = 1;
    auto w = static_cast<std::size_t>(window);
    return align_trailing(statcpp::rolling_mean(values, w), w, nulls);
}

static std::vector<double> wf_rolling_std(const std::vector<double>& values,
                                           const std::vector<bool>& nulls,
                                           int window) {
    if (window <= 0) window = 1;
    auto w = static_cast<std::size_t>(window);
    return align_trailing(statcpp::rolling_std(values, w), w, nulls);
}

static std::vector<double> wf_rolling_min(const std::vector<double>& values,
                                           const std::vector<bool>& nulls,
                                           int window) {
    if (window <= 0) window = 1;
    auto w = static_cast<std::size_t>(window);
    return align_trailing(statcpp::rolling_min(values, w), w, nulls);
}

static std::vector<double> wf_rolling_max(const std::vector<double>& values,
                                           const std::vector<bool>& nulls,
                                           int window) {
    if (window <= 0) window = 1;
    auto w = static_cast<std::size_t>(window);
    return align_trailing(statcpp::rolling_max(values, w), w, nulls);
}

static std::vector<double> wf_rolling_sum(const std::vector<double>& values,
                                           const std::vector<bool>& nulls,
                                           int window) {
    if (window <= 0) window = 1;
    auto w = static_cast<std::size_t>(window);
    return align_trailing(statcpp::rolling_sum(values, w), w, nulls);
}

// stat_moving_avg equals stat_rolling_mean on finite data. statcpp::moving_average
// sums each window separately (as R's stats::filter), so an Inf affects only the
// windows that contain it, while rolling_mean's running sum stays Inf/NaN after it.
static std::vector<double> wf_moving_avg(const std::vector<double>& values,
                                          const std::vector<bool>& nulls,
                                          int window) {
    if (window <= 0) window = 1;
    auto w = static_cast<std::size_t>(window);
    return align_trailing(statcpp::moving_average(values.begin(), values.end(), w), w, nulls);
}

static std::vector<double> wf_ema(const std::vector<double>& values,
                                   const std::vector<bool>& /*nulls*/,
                                   int span) {
    if (span <= 0) span = 10;
    double alpha = 2.0 / (static_cast<double>(span) + 1.0);
    auto result = statcpp::exponential_moving_average(values.begin(), values.end(), alpha);
    return result;
}

// --- Rank ---

static std::vector<double> wf_rank(const std::vector<double>& values,
                                    const std::vector<bool>& nulls,
                                    int /*param*/) {
    auto valid = extract_valid(values, nulls);
    if (valid.empty()) {
        return std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
    }
    auto ranks = statcpp::rank_transform(valid);
    // Map back to original positions
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = ranks[vi++];
        }
    }
    return result;
}

// --- Multiple testing corrections ---
//
// A correction value cannot be determined without seeing the whole set of p-values,
// because monotonicity has to be enforced. A scalar function evaluating one row at a
// time therefore cannot express it; these collect every row and delegate to statcpp.
// NULL rows are excluded from the correction and stay NULL in the output.

/**
 * @brief Map corrected p-values back onto their original row positions
 * @param values Collected values (NaN where the row was NULL)
 * @param nulls Whether each row was NULL
 * @param correction The statcpp correction function to apply
 * @return Corrected p-values in row order (NaN where the row was NULL)
 */
static std::vector<double> apply_pvalue_correction(
    const std::vector<double>& values,
    const std::vector<bool>& nulls,
    std::vector<double> (*correction)(const std::vector<double>&)) {
    auto valid = extract_valid(values, nulls);
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    if (valid.empty()) return result;
    auto adjusted = correction(valid);
    // Map back to original positions (same pattern as wf_rank)
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = adjusted[vi++];
        }
    }
    return result;
}

static std::vector<double> wf_bonferroni(const std::vector<double>& values,
                                          const std::vector<bool>& nulls,
                                          int /*param*/) {
    return apply_pvalue_correction(values, nulls, statcpp::bonferroni_correction);
}

static std::vector<double> wf_bh_correction(const std::vector<double>& values,
                                             const std::vector<bool>& nulls,
                                             int /*param*/) {
    return apply_pvalue_correction(values, nulls,
                                   statcpp::benjamini_hochberg_correction);
}

static std::vector<double> wf_holm_correction(const std::vector<double>& values,
                                               const std::vector<bool>& nulls,
                                               int /*param*/) {
    return apply_pvalue_correction(values, nulls, statcpp::holm_correction);
}

// --- fillna functions ---

static std::vector<double> wf_fillna_mean(const std::vector<double>& values,
                                            const std::vector<bool>& /*nulls*/,
                                            int /*param*/) {
    auto result = statcpp::fillna_mean(values);
    return result;
}

static std::vector<double> wf_fillna_median(const std::vector<double>& values,
                                              const std::vector<bool>& /*nulls*/,
                                              int /*param*/) {
    auto result = statcpp::fillna_median(values);
    return result;
}

static std::vector<double> wf_fillna_ffill(const std::vector<double>& values,
                                             const std::vector<bool>& /*nulls*/,
                                             int /*param*/) {
    auto result = statcpp::fillna_ffill(values);
    return result;
}

static std::vector<double> wf_fillna_bfill(const std::vector<double>& values,
                                             const std::vector<bool>& /*nulls*/,
                                             int /*param*/) {
    auto result = statcpp::fillna_bfill(values);
    return result;
}

static std::vector<double> wf_fillna_interp(const std::vector<double>& values,
                                              const std::vector<bool>& /*nulls*/,
                                              int /*param*/) {
    auto result = statcpp::fillna_interpolate(values);
    return result;
}

// --- Label encode ---

static std::vector<double> wf_label_encode(const std::vector<double>& values,
                                             const std::vector<bool>& nulls,
                                             int /*param*/) {
    auto valid = extract_valid(values, nulls);
    if (valid.empty()) {
        return std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
    }
    auto enc = statcpp::label_encode(valid);
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = static_cast<double>(enc.encoded[vi++]);
        }
    }
    return result;
}

// --- Binning ---

static std::vector<double> wf_bin_width(const std::vector<double>& values,
                                          const std::vector<bool>& nulls,
                                          int n_bins) {
    if (n_bins <= 0) n_bins = 10;
    auto valid = extract_valid(values, nulls);
    if (valid.empty()) {
        return std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
    }
    auto bins = statcpp::bin_equal_width(valid, static_cast<std::size_t>(n_bins));
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = static_cast<double>(bins[vi++]);
        }
    }
    return result;
}

static std::vector<double> wf_bin_freq(const std::vector<double>& values,
                                         const std::vector<bool>& nulls,
                                         int n_bins) {
    if (n_bins <= 0) n_bins = 10;
    auto valid = extract_valid(values, nulls);
    if (valid.empty()) {
        return std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
    }
    auto bins = statcpp::bin_equal_freq(valid, static_cast<std::size_t>(n_bins));
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = static_cast<double>(bins[vi++]);
        }
    }
    return result;
}

// --- Lag ---

static std::vector<double> wf_lag(const std::vector<double>& values,
                                   const std::vector<bool>& /*nulls*/,
                                   int k) {
    if (k <= 0) k = 1;
    auto result = statcpp::lag(values.begin(), values.end(),
                                static_cast<std::size_t>(k));
    // lag returns n-k elements (x[0] .. x[n-k-1]); pad front with NaN so that row i
    // holds x[i-k]. The SQLite version returned them unpadded, giving each row its
    // own value and leaving the last k rows NULL.
    std::vector<double> padded(values.size(), std::numeric_limits<double>::quiet_NaN());
    for (std::size_t i = 0; i < result.size(); ++i) {
        padded[i + static_cast<std::size_t>(k)] = result[i];
    }
    return padded;
}

// --- Outlier detection ---

static std::vector<double> wf_outliers_iqr(const std::vector<double>& values,
                                             const std::vector<bool>& nulls,
                                             int /*param*/) {
    auto valid = extract_valid(values, nulls);
    if (valid.empty()) {
        return std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
    }
    auto det = statcpp::detect_outliers_iqr(valid.begin(), valid.end());
    // Return 1.0 for outlier, 0.0 for normal, NaN for NULL
    // Build a set of outlier indices for lookup
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<bool> is_outlier(valid.size(), false);
    for (auto idx : det.outlier_indices) {
        if (idx < is_outlier.size()) is_outlier[idx] = true;
    }
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = is_outlier[vi] ? 1.0 : 0.0;
            vi++;
        }
    }
    return result;
}

static std::vector<double> wf_outliers_zscore(const std::vector<double>& values,
                                                const std::vector<bool>& nulls,
                                                int /*param*/) {
    auto valid = extract_valid(values, nulls);
    if (valid.empty()) {
        return std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
    }
    auto det = statcpp::detect_outliers_zscore(valid.begin(), valid.end());
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<bool> is_outlier(valid.size(), false);
    for (auto idx : det.outlier_indices) {
        if (idx < is_outlier.size()) is_outlier[idx] = true;
    }
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = is_outlier[vi] ? 1.0 : 0.0;
            vi++;
        }
    }
    return result;
}

static std::vector<double> wf_outliers_mzscore(const std::vector<double>& values,
                                                 const std::vector<bool>& nulls,
                                                 int /*param*/) {
    auto valid = extract_valid(values, nulls);
    if (valid.empty()) {
        return std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
    }
    auto det = statcpp::detect_outliers_modified_zscore(valid.begin(), valid.end());
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    std::vector<bool> is_outlier(valid.size(), false);
    for (auto idx : det.outlier_indices) {
        if (idx < is_outlier.size()) is_outlier[idx] = true;
    }
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = is_outlier[vi] ? 1.0 : 0.0;
            vi++;
        }
    }
    return result;
}

// --- Winsorize ---

static std::vector<double> wf_winsorize(const std::vector<double>& values,
                                          const std::vector<bool>& nulls,
                                          int /*param*/) {
    auto valid = extract_valid(values, nulls);
    if (valid.empty()) {
        return std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
    }
    auto wins = statcpp::winsorize(valid.begin(), valid.end());
    std::vector<double> result(values.size(), std::numeric_limits<double>::quiet_NaN());
    std::size_t vi = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nulls[i]) {
            result[i] = wins[vi++];
        }
    }
    return result;
}

// --- Diff ---

static std::vector<double> wf_diff(const std::vector<double>& values,
                                    const std::vector<bool>& /*nulls*/,
                                    int order) {
    if (order <= 0) order = 1;
    auto result = statcpp::diff(values.begin(), values.end(),
                                 static_cast<std::size_t>(order));
    // diff returns n-order elements; pad front with NaN
    std::vector<double> padded(values.size(), std::numeric_limits<double>::quiet_NaN());
    for (std::size_t i = 0; i < result.size(); ++i) {
        padded[i + static_cast<std::size_t>(order)] = result[i];
    }
    return padded;
}

static std::vector<double> wf_seasonal_diff(const std::vector<double>& values,
                                              const std::vector<bool>& /*nulls*/,
                                              int period) {
    if (period <= 0) period = 1;
    auto result = statcpp::seasonal_diff(values.begin(), values.end(),
                                          static_cast<std::size_t>(period));
    // seasonal_diff returns n-period elements; pad front with NaN
    std::vector<double> padded(values.size(), std::numeric_limits<double>::quiet_NaN());
    for (std::size_t i = 0; i < result.size(); ++i) {
        padded[i + static_cast<std::size_t>(period)] = result[i];
    }
    return padded;
}

// ===========================================================================
// Template E — SingleColumnAggregateText
//
// Like Template A but returns JSON text. Used for functions that collect a
// single column and return structured JSON results.
// ===========================================================================

template <std::string (*Func)(const std::vector<double>&)>
struct SingleColumnAggregateText {
    using State = SingleColumnState;

    static void Step(State& s, const ArgRow& argv) {
        if (argv.IsNull(0)) return;
        s.values.push_back(argv.Double(0));
    }

    static void Merge(State& t, const State& s) { t.values.insert(t.values.end(), s.values.begin(), s.values.end()); }

    static void Final(const State& s, ResultSlot& out) {
        if (s.values.empty()) {
            out.SetNull();
            return;
        }
        out.SetText(Func(s.values));
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        AggregateFunctionSet set(name);
        set.AddFunction(StatAggregate<SingleColumnAggregateText>::Make({LogicalType::DOUBLE}, LogicalType::VARCHAR));
        loader.RegisterFunction(set);
    }
};

// ===========================================================================
// Template F — ScalarFunction
//
// Scalar implementations keep the SQLite signature shape (ctx, argc, argv).
// Following SQL, a row with any NULL argument returns NULL without calling the
// implementation (guarded_scalar). An omitted optional argument still selects
// its default, through the overload for the shorter argument count.
// ===========================================================================

using ScalarImpl = void (*)(ResultSlot&, int, const ArgRow&);

/**
 * @brief DuckDB scalar callback that runs a scalar implementation row by row
 *
 * The implementation is taken as a non-type template parameter, which makes it
 * structurally impossible to register one without the exception guard.
 *
 * @tparam Fn The actual scalar function implementation
 */
/**
 * @brief Whether any argument of a scalar call is NULL
 * @param row Arguments of one row
 * @return true if at least one argument is NULL
 */
[[nodiscard]] static bool HasNullArgument(const ArgRow& row) {
    for (int i = 0; i < row.Count(); ++i) {
        if (row.IsNull(i)) return true;
    }
    return false;
}

template <ScalarImpl Fn>
static void guarded_scalar(DataChunk& args, ExpressionState&, Vector& result) {
    invoke_guarded([&] {
        const idx_t count = args.size();
        InputColumns columns(args.data.data(), args.ColumnCount(), count);
        result.SetVectorType(VectorType::FLAT_VECTOR);
        for (idx_t i = 0; i < count; ++i) {
            ResultSlot out(result, i);
            const ArgRow row(columns, i);
            if (HasNullArgument(row)) {
                out.SetNull();
                continue;
            }
            Fn(out, row.Count(), row);
        }
    });
}

/**
 * @brief Build a scalar function set with one overload per accepted argument count
 * @param name Function name as seen from SQL
 * @param arities Accepted argument counts (every argument is DOUBLE)
 * @param return_type Return type
 * @param is_volatile Whether the function draws random numbers
 */
template <ScalarImpl Fn>
static ScalarFunctionSet make_scalar(const char* name, std::initializer_list<idx_t> arities,
                                     const LogicalType& return_type, bool is_volatile) {
    ScalarFunctionSet set(name);
    for (idx_t n : arities) {
        ScalarFunction fn(duckdb::vector<LogicalType>(n, LogicalType::DOUBLE), return_type, guarded_scalar<Fn>);
        fn.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
        if (is_volatile) fn.SetVolatile();
        set.AddFunction(fn);
    }
    return set;
}

/**
 * @brief Register a scalar function (one whose result depends only on its arguments)
 * @param loader Extension loader
 * @param name Function name as seen from SQL
 * @param arities Accepted argument counts
 * @param return_type Return type (DOUBLE unless stated)
 */
template <ScalarImpl Fn>
static void register_scalar(ExtensionLoader& loader, const char* name, std::initializer_list<idx_t> arities,
                            const LogicalType& return_type = LogicalType::DOUBLE) {
    loader.RegisterFunction(make_scalar<Fn>(name, arities, return_type, false));
}

/**
 * @brief Register a non-deterministic scalar function (one that draws random numbers)
 *
 * Registered as VOLATILE so DuckDB neither constant-folds it nor reuses one
 * value for every row.
 *
 * @param loader Extension loader
 * @param name Function name as seen from SQL
 * @param arities Accepted argument counts
 * @param return_type Return type (DOUBLE unless stated)
 */
template <ScalarImpl Fn>
static void register_scalar_nd(ExtensionLoader& loader, const char* name, std::initializer_list<idx_t> arities,
                               const LogicalType& return_type = LogicalType::DOUBLE) {
    loader.RegisterFunction(make_scalar<Fn>(name, arities, return_type, true));
}

// Result helpers, named as in the SQLite version

/// @brief Set a double result (NaN/Inf -> NULL)
static void result_double_or_null(ResultSlot& ctx, double v) { ctx.SetDouble(v); }

/// @brief Set an int64 result
static void result_int64(ResultSlot& ctx, std::int64_t v) { ctx.SetInt64(v); }

/// @brief Set a text result (empty -> NULL)
static void result_text(ResultSlot& ctx, const std::string& s) { ctx.SetText(s); }

/**
 * @brief Return a quantile for a distribution with unbounded support, or NULL
 *
 * The Poisson, geometric and negative binomial distributions are unbounded above,
 * so no finite quantile corresponds to p = 1. statcpp returns the maximum uint64
 * value as a sentinel in that case, which maps to NULL in SQL.
 *
 * @param ctx Result cell
 * @param v Return value of the statcpp quantile function
 */
static void result_unbounded_quantile(ResultSlot& ctx, std::uint64_t v) {
    if (v == std::numeric_limits<std::uint64_t>::max()) {
        ctx.SetNull();
    } else {
        ctx.SetInt64(static_cast<std::int64_t>(v));
    }
}

// ===========================================================================
// Complex aggregate wrapper functions — two-sample / JSON
// ===========================================================================

// --- P5-1: stat_modes (single column → JSON array of modes) ---
static std::string calc_modes(const std::vector<double>& v) {
    auto m = statcpp::modes(v.begin(), v.end());
    std::string s = "[";
    for (std::size_t i = 0; i < m.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(m[i]);
    }
    s += "]";
    return s;
}

// --- P5-2: stat_five_number_summary ---
static std::string calc_five_number_summary(const std::vector<double>& v) {
    std::vector<double> sorted(v);
    std::sort(sorted.begin(), sorted.end());
    double mn = sorted.front();
    auto qr = statcpp::quartiles(sorted.begin(), sorted.end());
    double q1 = qr.q1;
    double med = qr.q2;
    double q3 = qr.q3;
    double mx = sorted.back();
    return "{\"min\":" + json_double(mn)
         + ",\"q1\":" + json_double(q1)
         + ",\"median\":" + json_double(med)
         + ",\"q3\":" + json_double(q3)
         + ",\"max\":" + json_double(mx) + "}";
}

// --- P5-3: stat_frequency_table ---
static std::string calc_frequency_table(const std::vector<double>& v) {
    auto ft = statcpp::frequency_table(v.begin(), v.end());
    std::string s = "[";
    for (std::size_t i = 0; i < ft.entries.size(); ++i) {
        if (i > 0) s += ",";
        s += "{\"value\":" + json_double(ft.entries[i].value)
           + ",\"count\":" + std::to_string(ft.entries[i].count)
           + ",\"relative_frequency\":" + json_double(ft.entries[i].relative_frequency) + "}";
    }
    s += "]";
    return s;
}

// --- P5-4: stat_frequency_count (single column → JSON {value:count,...}) ---
static std::string calc_frequency_count(const std::vector<double>& v) {
    auto fc = statcpp::frequency_count(v.begin(), v.end());
    std::string s = "{";
    bool first = true;
    for (auto& p : fc) {
        if (!first) s += ",";
        first = false;
        s += "\"" + json_double(p.first) + "\":" + std::to_string(p.second);
    }
    s += "}";
    return s;
}

// --- P5-5: stat_relative_frequency ---
static std::string calc_relative_frequency(const std::vector<double>& v) {
    auto rf = statcpp::relative_frequency(v.begin(), v.end());
    std::string s = "{";
    bool first = true;
    for (auto& p : rf) {
        if (!first) s += ",";
        first = false;
        s += "\"" + json_double(p.first) + "\":" + json_double(p.second);
    }
    s += "}";
    return s;
}

// --- P5-6: stat_cumulative_frequency ---
static std::string calc_cumulative_frequency(const std::vector<double>& v) {
    auto cf = statcpp::cumulative_frequency(v.begin(), v.end());
    std::string s = "{";
    bool first = true;
    for (auto& p : cf) {
        if (!first) s += ",";
        first = false;
        s += "\"" + json_double(p.first) + "\":" + std::to_string(p.second);
    }
    s += "}";
    return s;
}

// --- P5-7: stat_cumulative_relative_frequency ---
static std::string calc_cumulative_relative_frequency(const std::vector<double>& v) {
    auto crf = statcpp::cumulative_relative_frequency(v.begin(), v.end());
    std::string s = "{";
    bool first = true;
    for (auto& p : crf) {
        if (!first) s += ",";
        first = false;
        s += "\"" + json_double(p.first) + "\":" + json_double(p.second);
    }
    s += "}";
    return s;
}

// --- P5-8..12: Two-sample tests (use TwoColumnAggregateText) ---

static std::string calc_t_test2(const std::vector<double>& x,
                                 const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return "";
    auto r = statcpp::t_test_two_sample(x.begin(), x.end(), y.begin(), y.end());
    return json_test_result(r);
}

static std::string calc_t_test_welch(const std::vector<double>& x,
                                      const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return "";
    auto r = statcpp::t_test_welch(x.begin(), x.end(), y.begin(), y.end());
    return json_test_result(r);
}

static std::string calc_chisq_independence(const std::vector<double>& x,
                                            const std::vector<double>& y) {
    if (x.size() < 2) return "";
    // Build a 2-column contingency table from x and y values
    // x = row category, y = col category
    std::map<double, std::size_t> row_map, col_map;
    for (auto v : x) row_map.insert({v, row_map.size()});
    for (auto v : y) col_map.insert({v, col_map.size()});
    std::vector<std::vector<double>> table(row_map.size(),
        std::vector<double>(col_map.size(), 0.0));
    for (std::size_t i = 0; i < x.size(); ++i) {
        table[row_map[x[i]]][col_map[y[i]]] += 1.0;
    }
    auto r = statcpp::chisq_test_independence(table);
    return json_test_result(r);
}

static std::string calc_f_test(const std::vector<double>& x,
                                const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return "";
    auto r = statcpp::f_test(x.begin(), x.end(), y.begin(), y.end());
    return json_test_result(r);
}

static std::string calc_mann_whitney(const std::vector<double>& x,
                                      const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return "";
    auto r = statcpp::mann_whitney_u_test(x.begin(), x.end(), y.begin(), y.end());
    return json_test_result(r);
}

// ===========================================================================
// Shared handling for the group-column form
//
// Takes the two columns "values + groups" and converts them into the
// std::vector<std::vector<double>> statcpp expects. Groups come out in ascending
// order of the group column (std::map ordering), so a group's index matches the
// ascending order of its value on the SQL side.
// ===========================================================================

/**
 * @brief Split a value column by a group column
 * @param values The values
 * @param groups Column identifying the group (any values; indices are assigned in ascending order)
 * @return Values per group, or empty if there are fewer than two groups
 */
static std::vector<std::vector<double>> split_by_group(
    const std::vector<double>& values,
    const std::vector<double>& groups) {
    std::map<double, std::vector<double>> grouped;
    for (std::size_t i = 0; i < values.size(); ++i) {
        grouped[groups[i]].push_back(values[i]);
    }
    std::vector<std::vector<double>> group_vecs;
    group_vecs.reserve(grouped.size());
    for (auto& g : grouped) group_vecs.push_back(std::move(g.second));
    // Every test and analysis of variance here needs at least two groups
    if (group_vecs.size() < 2) return {};
    return group_vecs;
}

/**
 * @brief Validate an optional alpha, falling back to the default of 0.05
 * @param raw Value passed from SQL (0.0 when called in the two-argument form)
 * @return raw if it lies in (0, 1), otherwise 0.05
 */
static double resolve_alpha(double raw) {
    return (raw > 0.0 && raw < 1.0) ? raw : 0.05;
}

// --- P5-13: stat_anova1 (two-column: value, group → JSON) ---
static std::string calc_anova1(const std::vector<double>& values,
                                const std::vector<double>& groups) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return "";
    auto r = statcpp::one_way_anova(group_vecs);
    return "{\"f_statistic\":" + json_double(r.between.f_statistic)
         + ",\"p_value\":" + json_double(r.between.p_value)
         + ",\"df_between\":" + json_double(r.between.df)
         + ",\"df_within\":" + json_double(r.within.df)
         + ",\"ss_between\":" + json_double(r.between.ss)
         + ",\"ss_within\":" + json_double(r.within.ss)
         + ",\"n_groups\":" + std::to_string(r.n_groups) + "}";
}

// ===========================================================================
// Tests and post-hoc tests over a group column
//
// All take "values + groups", split them with split_by_group() and delegate to
// statcpp. The call form is the same as stat_anova1.
// ===========================================================================

// --- Tests comparing groups (value, group -> JSON) ---

/// @brief Kruskal-Wallis test (the nonparametric counterpart of one-way ANOVA)
static std::string calc_kruskal_wallis(const std::vector<double>& values,
                                        const std::vector<double>& groups) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return "";
    return json_test_result(statcpp::kruskal_wallis_test(group_vecs));
}

/// @brief Levene test (equal variance; preferred when normality is doubtful)
static std::string calc_levene(const std::vector<double>& values,
                                const std::vector<double>& groups) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return "";
    return json_test_result(statcpp::levene_test(group_vecs));
}

/// @brief Bartlett test (equal variance; assumes normality)
static std::string calc_bartlett(const std::vector<double>& values,
                                  const std::vector<double>& groups) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return "";
    return json_test_result(statcpp::bartlett_test(group_vecs));
}

/// @brief Cohen's f (the one-way ANOVA effect size)
static double calc_cohens_f(const std::vector<double>& values,
                             const std::vector<double>& groups) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::cohens_f(statcpp::one_way_anova(group_vecs));
}

// --- Post-hoc tests (value, group [,alpha] -> JSON) ---

/**
 * @brief Convert a posthoc_result to JSON
 *
 * group1 and group2 are zero-based indices into the group column's values taken
 * in ascending order.
 */
static std::string json_posthoc_result(const statcpp::posthoc_result& r) {
    std::string out = "{\"method\":\"" + r.method + "\"";
    out += ",\"alpha\":" + json_double(r.alpha);
    out += ",\"mse\":" + json_double(r.mse);
    out += ",\"df_error\":" + json_double(r.df_error);
    out += ",\"comparisons\":[";
    for (std::size_t i = 0; i < r.comparisons.size(); ++i) {
        const auto& c = r.comparisons[i];
        if (i > 0) out += ",";
        out += "{\"group1\":" + std::to_string(c.group1)
             + ",\"group2\":" + std::to_string(c.group2)
             + ",\"mean_diff\":" + json_double(c.mean_diff)
             + ",\"se\":" + json_double(c.se)
             + ",\"statistic\":" + json_double(c.statistic)
             + ",\"p_value\":" + json_double(c.p_value)
             + ",\"lower\":" + json_double(c.lower)
             + ",\"upper\":" + json_double(c.upper)
             + ",\"significant\":" + (c.significant ? "true" : "false") + "}";
    }
    out += "]}";
    return out;
}

/// @brief Tukey HSD (all pairs; the standard post-hoc test after ANOVA)
static std::string calc_tukey_hsd(const std::vector<double>& values,
                                   const std::vector<double>& groups,
                                   const std::array<double, 1>& p) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return "";
    auto anova = statcpp::one_way_anova(group_vecs);
    return json_posthoc_result(
        statcpp::tukey_hsd(anova, group_vecs, resolve_alpha(p[0])));
}

/// @brief Bonferroni post-hoc test (all pairs; conservative)
static std::string calc_bonferroni_posthoc(const std::vector<double>& values,
                                            const std::vector<double>& groups,
                                            const std::array<double, 1>& p) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return "";
    auto anova = statcpp::one_way_anova(group_vecs);
    return json_posthoc_result(
        statcpp::bonferroni_posthoc(anova, resolve_alpha(p[0])));
}

/// @brief Scheffe post-hoc test (all pairs; the most conservative)
static std::string calc_scheffe_posthoc(const std::vector<double>& values,
                                         const std::vector<double>& groups,
                                         const std::array<double, 1>& p) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return "";
    auto anova = statcpp::one_way_anova(group_vecs);
    return json_posthoc_result(
        statcpp::scheffe_posthoc(anova, resolve_alpha(p[0])));
}

/// @brief Dunnett post-hoc test (against a control group only)
/// @param p [0] = control group index (default 0), [1] = alpha (default 0.05)
static std::string calc_dunnett_posthoc(const std::vector<double>& values,
                                         const std::vector<double>& groups,
                                         const std::array<double, 2>& p) {
    auto group_vecs = split_by_group(values, groups);
    if (group_vecs.empty()) return "";
    auto anova = statcpp::one_way_anova(group_vecs);
    auto control = static_cast<std::size_t>(p[0] > 0.0 ? p[0] : 0.0);
    if (control >= group_vecs.size()) return "";
    return json_posthoc_result(
        statcpp::dunnett_posthoc(anova, control, resolve_alpha(p[1])));
}

// --- Stratified sampling (value, group [,ratio] -> JSON array) ---

/// @brief Stratified random sample, drawing the same proportion from each stratum
/// @param p [0] = sampling ratio in (0,1]; 0.5 when omitted or out of range
static std::string calc_stratified_sample(const std::vector<double>& values,
                                           const std::vector<double>& groups,
                                           const std::array<double, 1>& p) {
    double ratio = (p[0] > 0.0 && p[0] <= 1.0) ? p[0] : 0.5;
    // statcpp::stratified_sample takes (strata, data, ratio) as parallel arrays
    auto sampled = statcpp::stratified_sample(groups, values, ratio);
    std::string out = "[";
    for (std::size_t i = 0; i < sampled.size(); ++i) {
        if (i > 0) out += ",";
        out += json_double(sampled[i]);
    }
    out += "]";
    return out;
}

// --- P5-14: stat_contingency_table ---
static std::string calc_contingency_table(const std::vector<double>& x,
                                           const std::vector<double>& y) {
    // Convert to size_t indices
    std::map<double, std::size_t> row_map, col_map;
    std::vector<std::size_t> rows, cols;
    for (auto v : x) {
        auto it = row_map.insert({v, row_map.size()});
        rows.push_back(it.first->second);
    }
    for (auto v : y) {
        auto it = col_map.insert({v, col_map.size()});
        cols.push_back(it.first->second);
    }
    auto ct = statcpp::contingency_table(rows, cols);
    // Output as JSON 2D array
    std::string s = "{\"table\":[";
    for (std::size_t r = 0; r < ct.n_rows; ++r) {
        if (r > 0) s += ",";
        s += "[";
        for (std::size_t c = 0; c < ct.n_cols; ++c) {
            if (c > 0) s += ",";
            s += std::to_string(ct.table[r][c]);
        }
        s += "]";
    }
    s += "],\"total\":" + std::to_string(ct.total) + "}";
    return s;
}

// --- P5-15..17: Two-sample effect sizes ---
static double calc_cohens_d2(const std::vector<double>& x,
                              const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::cohens_d_two_sample(x.begin(), x.end(), y.begin(), y.end());
}

static double calc_hedges_g2(const std::vector<double>& x,
                              const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::hedges_g_two_sample(x.begin(), x.end(), y.begin(), y.end());
}

static double calc_glass_delta(const std::vector<double>& x,
                                const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    return statcpp::glass_delta(x.begin(), x.end(), y.begin(), y.end());
}

// --- P5-18..19: CI for mean difference ---
static std::string calc_ci_mean_diff(const std::vector<double>& x,
                                      const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return "";
    auto ci = statcpp::ci_mean_diff(x.begin(), x.end(), y.begin(), y.end());
    return json_ci(ci);
}

static std::string calc_ci_mean_diff_welch(const std::vector<double>& x,
                                            const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return "";
    auto ci = statcpp::ci_mean_diff_welch(x.begin(), x.end(), y.begin(), y.end());
    return json_ci(ci);
}

// --- P5-20: stat_kaplan_meier (two-column: time, event → JSON) ---
static std::string calc_kaplan_meier(const std::vector<double>& times,
                                      const std::vector<double>& events) {
    std::vector<bool> ev;
    ev.reserve(events.size());
    for (auto e : events) ev.push_back(e != 0.0);
    auto km = statcpp::kaplan_meier(times, ev);
    std::string s = "{\"times\":[";
    for (std::size_t i = 0; i < km.times.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(km.times[i]);
    }
    s += "],\"survival\":[";
    for (std::size_t i = 0; i < km.survival.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(km.survival[i]);
    }
    s += "],\"se\":[";
    for (std::size_t i = 0; i < km.se.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(km.se[i]);
    }
    s += "]}";
    return s;
}

// --- P5-21: stat_logrank (needs 4 columns — we use col1=time, col2=event_or_group) ---
// For logrank, we split by group: col1=time, col2=event(0/1)*10+group(0/1)
// Simpler: col1=time, col2=group. Event is assumed=1 for all (no censoring in basic usage).
// OR: we register as 3-arg aggregate: time, event, group
// Let's use a direct registration with 3 columns.
// We'll handle this with a custom registration.

// --- P5-22: stat_nelson_aalen ---
static std::string calc_nelson_aalen(const std::vector<double>& times,
                                      const std::vector<double>& events) {
    std::vector<bool> ev;
    ev.reserve(events.size());
    for (auto e : events) ev.push_back(e != 0.0);
    auto na = statcpp::nelson_aalen(times, ev);
    std::string s = "{\"times\":[";
    for (std::size_t i = 0; i < na.times.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(na.times[i]);
    }
    s += "],\"cumulative_hazard\":[";
    for (std::size_t i = 0; i < na.cumulative_hazard.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(na.cumulative_hazard[i]);
    }
    s += "]}";
    return s;
}

// --- P5-23: stat_bootstrap (single column → JSON, uses mean as default statistic) ---
static std::string calc_bootstrap_generic(const std::vector<double>& v,
                                           const std::array<double, 1>& p) {
    auto n = static_cast<std::size_t>(p[0]);
    if (n < 2) n = 1000;
    auto stat_func = [](auto f, auto l) { return statcpp::mean(f, l); };
    auto r = statcpp::bootstrap(v.begin(), v.end(), stat_func, n);
    return json_bootstrap(r);
}

// --- P5-24: stat_bootstrap_bca ---
static std::string calc_bootstrap_bca(const std::vector<double>& v,
                                       const std::array<double, 1>& p) {
    if (v.size() < 3) return "";
    auto n = static_cast<std::size_t>(p[0]);
    if (n < 2) n = 1000;
    auto stat_func = [](auto f, auto l) { return statcpp::mean(f, l); };
    auto r = statcpp::bootstrap_bca(v.begin(), v.end(), stat_func, n);
    return json_bootstrap(r);
}

// --- P5-25: stat_bootstrap_sample (returns JSON array of sampled values) ---
static std::string calc_bootstrap_sample_text(const std::vector<double>& v,
                                               const std::array<double, 0>&) {
    auto sample = statcpp::bootstrap_sample(v.begin(), v.end());
    std::string s = "[";
    for (std::size_t i = 0; i < sample.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(sample[i]);
    }
    s += "]";
    return s;
}

// --- P5-26..28: Permutation tests (two-column) ---
static std::string json_permutation(const statcpp::permutation_result& r) {
    return "{\"observed_statistic\":" + json_double(r.observed_statistic)
         + ",\"p_value\":" + json_double(r.p_value)
         + ",\"n_permutations\":" + std::to_string(r.n_permutations) + "}";
}

static std::string calc_permutation_test2(const std::vector<double>& x,
                                           const std::vector<double>& y) {
    if (x.size() < 2 || y.size() < 2) return "";
    auto r = statcpp::permutation_test_two_sample(x.begin(), x.end(),
                                                    y.begin(), y.end(), 1000);
    return json_permutation(r);
}

static std::string calc_permutation_paired(const std::vector<double>& x,
                                            const std::vector<double>& y) {
    if (x.size() < 2) return "";
    auto r = statcpp::permutation_test_paired(x.begin(), x.end(),
                                               y.begin(), y.end(), 1000);
    return json_permutation(r);
}

static std::string calc_permutation_corr(const std::vector<double>& x,
                                          const std::vector<double>& y) {
    if (x.size() < 3) return "";
    auto r = statcpp::permutation_test_correlation(x.begin(), x.end(),
                                                    y.begin(), y.end(), 1000);
    return json_permutation(r);
}

// --- P5-29..30: ACF / PACF (single column + max_lag param → JSON array) ---
static std::string calc_acf(const std::vector<double>& v,
                              const std::array<double, 1>& p) {
    auto max_lag = static_cast<std::size_t>(p[0]);
    if (max_lag == 0 || max_lag >= v.size()) max_lag = v.size() / 2;
    auto acf_vals = statcpp::acf(v.begin(), v.end(), max_lag);
    std::string s = "[";
    for (std::size_t i = 0; i < acf_vals.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(acf_vals[i]);
    }
    s += "]";
    return s;
}

static std::string calc_pacf(const std::vector<double>& v,
                               const std::array<double, 1>& p) {
    auto max_lag = static_cast<std::size_t>(p[0]);
    if (max_lag == 0 || max_lag >= v.size()) max_lag = v.size() / 2;
    auto pacf_vals = statcpp::pacf(v.begin(), v.end(), max_lag);
    std::string s = "[";
    for (std::size_t i = 0; i < pacf_vals.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(pacf_vals[i]);
    }
    s += "]";
    return s;
}

// --- P5-31..32: Sampling ---
static std::string calc_sample_replace(const std::vector<double>& v,
                                        const std::array<double, 1>& p) {
    auto n = static_cast<std::size_t>(p[0]);
    if (n == 0) n = v.size();
    auto sample = statcpp::sample_with_replacement(v, n);
    std::string s = "[";
    for (std::size_t i = 0; i < sample.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(sample[i]);
    }
    s += "]";
    return s;
}

static std::string calc_sample_noreplace(const std::vector<double>& v,
                                          const std::array<double, 1>& p) {
    auto n = static_cast<std::size_t>(p[0]);
    if (n == 0 || n > v.size()) n = v.size();
    auto sample = statcpp::sample_without_replacement(v, n);
    std::string s = "[";
    for (std::size_t i = 0; i < sample.size(); ++i) {
        if (i > 0) s += ",";
        s += json_double(sample[i]);
    }
    s += "]";
    return s;
}

// --- logrank: three columns (time, event, group) ---
struct ThreeColumnState {
    std::vector<double> c1, c2, c3;
};

struct LogrankAggregate {
    using State = ThreeColumnState;

    static void Step(State& s, const ArgRow& argv) {
        if (argv.IsNull(0) || argv.IsNull(1) || argv.IsNull(2)) return;
        s.c1.push_back(argv.Double(0)); // time
        s.c2.push_back(argv.Double(1)); // event
        s.c3.push_back(argv.Double(2)); // group
    }

    static void Merge(State& t, const State& s) {
        t.c1.insert(t.c1.end(), s.c1.begin(), s.c1.end());
        t.c2.insert(t.c2.end(), s.c2.begin(), s.c2.end());
        t.c3.insert(t.c3.end(), s.c3.begin(), s.c3.end());
    }

    static void Final(const State& st, ResultSlot& out) {
        if (st.c1.empty()) {
            out.SetNull();
            return;
        }
        // Split by group (0 vs non-0)
        std::vector<double> t1, t2;
        std::vector<bool> e1, e2;
        for (std::size_t i = 0; i < st.c1.size(); ++i) {
            if (st.c3[i] == 0.0) {
                t1.push_back(st.c1[i]);
                e1.push_back(st.c2[i] != 0.0);
            } else {
                t2.push_back(st.c1[i]);
                e2.push_back(st.c2[i] != 0.0);
            }
        }
        if (t1.empty() || t2.empty()) { out.SetNull(); return; }
        auto r = statcpp::logrank_test(t1, e1, t2, e2);
        std::string s = "{\"statistic\":" + json_double(r.statistic)
                      + ",\"p_value\":" + json_double(r.p_value)
                      + ",\"df\":" + std::to_string(r.df) + "}";
        out.SetText(s);
    }

    static void register_func(ExtensionLoader& loader, const char* name) {
        AggregateFunctionSet set(name);
        set.AddFunction(StatAggregate<LogrankAggregate>::Make(
            {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::VARCHAR));
        loader.RegisterFunction(set);
    }
};

// ===========================================================================
// Scalar functions — tests and helpers
// ===========================================================================

// --- P6-1..16: Distribution functions (normal, chi-sq, t, F) ---

static void sf_normal_pdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double mu = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double sigma = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::normal_pdf(x, mu, sigma));
}
static void sf_normal_cdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double mu = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double sigma = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::normal_cdf(x, mu, sigma));
}
static void sf_normal_quantile(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double p = argv.Double(0);
    double mu = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double sigma = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::normal_quantile(p, mu, sigma));
}
static void sf_normal_rand(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double mu = (argc > 0 && !argv.IsNull(0)) ? argv.Double(0) : 0.0;
    double sigma = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 1.0;
    result_double_or_null(ctx, statcpp::normal_rand(mu, sigma));
}

static void sf_chisq_pdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::chisq_pdf(argv.Double(0),
                                                    argv.Double(1)));
}
static void sf_chisq_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::chisq_cdf(argv.Double(0),
                                                    argv.Double(1)));
}
static void sf_chisq_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::chisq_quantile(argv.Double(0),
                                                        argv.Double(1)));
}
static void sf_chisq_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::chisq_rand(argv.Double(0)));
}

static void sf_t_pdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::t_pdf(argv.Double(0),
                                               argv.Double(1)));
}
static void sf_t_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::t_cdf(argv.Double(0),
                                               argv.Double(1)));
}
static void sf_t_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::t_quantile(argv.Double(0),
                                                    argv.Double(1)));
}
static void sf_t_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::t_rand(argv.Double(0)));
}

static void sf_f_pdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::f_pdf(argv.Double(0),
                                               argv.Double(1),
                                               argv.Double(2)));
}
static void sf_f_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::f_cdf(argv.Double(0),
                                               argv.Double(1),
                                               argv.Double(2)));
}
static void sf_f_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::f_quantile(argv.Double(0),
                                                    argv.Double(1),
                                                    argv.Double(2)));
}
static void sf_f_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::f_rand(argv.Double(0),
                                                argv.Double(1)));
}

// --- P6-17..23: Special functions ---
static void sf_betainc(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::betainc(argv.Double(0),
                                                 argv.Double(1),
                                                 argv.Double(2)));
}
static void sf_betaincinv(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::betaincinv(argv.Double(0),
                                                    argv.Double(1),
                                                    argv.Double(2)));
}
static void sf_norm_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::norm_cdf(argv.Double(0)));
}
static void sf_norm_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::norm_quantile(argv.Double(0)));
}
static void sf_gammainc_lower(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::gammainc_lower(argv.Double(0),
                                                        argv.Double(1)));
}
static void sf_gammainc_upper(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::gammainc_upper(argv.Double(0),
                                                        argv.Double(1)));
}
static void sf_gammainc_lower_inv(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::gammainc_lower_inv(argv.Double(0),
                                                            argv.Double(1)));
}

// --- P6-24..25: Proportion z-tests ---
static void sf_z_test_prop(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto succ = static_cast<std::size_t>(argv.Int64(0));
    auto trials = static_cast<std::size_t>(argv.Int64(1));
    double p0 = argv.Double(2);
    auto r = statcpp::z_test_proportion(succ, trials, p0);
    std::string s = json_test_result(r);
    result_text(ctx, s);
}
static void sf_z_test_prop2(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto s1 = static_cast<std::size_t>(argv.Int64(0));
    auto n1 = static_cast<std::size_t>(argv.Int64(1));
    auto s2 = static_cast<std::size_t>(argv.Int64(2));
    auto n2 = static_cast<std::size_t>(argv.Int64(3));
    auto r = statcpp::z_test_proportion_two_sample(s1, n1, s2, n2);
    std::string s = json_test_result(r);
    result_text(ctx, s);
}

// --- P6-26: Bonferroni correction (scalar form) ---
// Adjusts a single p-value when the number of tests m is known. min(p*m, 1) is the
// same expression statcpp's bonferroni_correction uses, and needs no monotonicity
// step, so it can be expressed as a scalar. To correct a whole set of p-values, use
// the window form stat_bonferroni(p) OVER () instead.
//
// BH and Holm corrections need the entire set of p-values to enforce monotonicity, so
// no scalar form is offered for them; only the window functions stat_bh_correction
// and stat_holm_correction.
static void sf_bonferroni(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double p = argv.Double(0);
    int m = argv.Int(1);
    double adj = std::min(p * m, 1.0);
    result_double_or_null(ctx, adj);
}

// --- P6-29: Fisher exact test ---
static void sf_fisher_exact(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto a = static_cast<std::uint64_t>(argv.Int64(0));
    auto b = static_cast<std::uint64_t>(argv.Int64(1));
    auto c = static_cast<std::uint64_t>(argv.Int64(2));
    auto d = static_cast<std::uint64_t>(argv.Int64(3));
    auto r = statcpp::fisher_exact_test(a, b, c, d);
    std::string s = json_test_result(r);
    result_text(ctx, s);
}

// --- P6-30..33: Categorical measures ---
static void sf_odds_ratio(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto a = static_cast<std::size_t>(argv.Int64(0));
    auto b = static_cast<std::size_t>(argv.Int64(1));
    auto c = static_cast<std::size_t>(argv.Int64(2));
    auto d = static_cast<std::size_t>(argv.Int64(3));
    auto r = statcpp::odds_ratio(a, b, c, d);
    std::string s = "{\"odds_ratio\":" + json_double(r.odds_ratio)
                  + ",\"ci_lower\":" + json_double(r.ci_lower)
                  + ",\"ci_upper\":" + json_double(r.ci_upper) + "}";
    result_text(ctx, s);
}
static void sf_relative_risk(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto a = static_cast<std::size_t>(argv.Int64(0));
    auto b = static_cast<std::size_t>(argv.Int64(1));
    auto c = static_cast<std::size_t>(argv.Int64(2));
    auto d = static_cast<std::size_t>(argv.Int64(3));
    auto r = statcpp::relative_risk(a, b, c, d);
    std::string s = "{\"relative_risk\":" + json_double(r.relative_risk)
                  + ",\"ci_lower\":" + json_double(r.ci_lower)
                  + ",\"ci_upper\":" + json_double(r.ci_upper) + "}";
    result_text(ctx, s);
}
static void sf_risk_difference(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto a = static_cast<std::size_t>(argv.Int64(0));
    auto b = static_cast<std::size_t>(argv.Int64(1));
    auto c = static_cast<std::size_t>(argv.Int64(2));
    auto d = static_cast<std::size_t>(argv.Int64(3));
    auto r = statcpp::risk_difference(a, b, c, d);
    std::string s = "{\"risk_difference\":" + json_double(r.risk_difference)
                  + ",\"ci_lower\":" + json_double(r.ci_lower)
                  + ",\"ci_upper\":" + json_double(r.ci_upper) + "}";
    result_text(ctx, s);
}
static void sf_nnt(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto a = static_cast<std::size_t>(argv.Int64(0));
    auto b = static_cast<std::size_t>(argv.Int64(1));
    auto c = static_cast<std::size_t>(argv.Int64(2));
    auto d = static_cast<std::size_t>(argv.Int64(3));
    std::vector<std::vector<std::size_t>> table = {{a, b}, {c, d}};
    double nnt = statcpp::number_needed_to_treat(table);
    result_double_or_null(ctx, nnt);
}

// --- P6-34..36: Proportion confidence intervals ---
static void sf_ci_prop(ResultSlot& ctx, int argc, const ArgRow& argv) {
    auto succ = static_cast<std::size_t>(argv.Int64(0));
    auto trials = static_cast<std::size_t>(argv.Int64(1));
    double conf = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 0.95;
    auto ci = statcpp::ci_proportion(succ, trials, conf);
    std::string s = json_ci(ci);
    result_text(ctx, s);
}
static void sf_ci_prop_wilson(ResultSlot& ctx, int argc, const ArgRow& argv) {
    auto succ = static_cast<std::size_t>(argv.Int64(0));
    auto trials = static_cast<std::size_t>(argv.Int64(1));
    double conf = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 0.95;
    auto ci = statcpp::ci_proportion_wilson(succ, trials, conf);
    std::string s = json_ci(ci);
    result_text(ctx, s);
}
static void sf_ci_prop_diff(ResultSlot& ctx, int argc, const ArgRow& argv) {
    auto s1 = static_cast<std::size_t>(argv.Int64(0));
    auto n1 = static_cast<std::size_t>(argv.Int64(1));
    auto s2 = static_cast<std::size_t>(argv.Int64(2));
    auto n2 = static_cast<std::size_t>(argv.Int64(3));
    double conf = (argc > 4 && !argv.IsNull(4)) ? argv.Double(4) : 0.95;
    auto ci = statcpp::ci_proportion_diff(s1, n1, s2, n2, conf);
    std::string s = json_ci(ci);
    result_text(ctx, s);
}

// --- P6-37..39: Model selection ---
static void sf_aic(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double ll = argv.Double(0);
    auto k = static_cast<std::size_t>(argv.Int64(1));
    result_double_or_null(ctx, statcpp::aic(ll, k));
}
static void sf_aicc(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double ll = argv.Double(0);
    auto n = static_cast<std::size_t>(argv.Int64(1));
    auto k = static_cast<std::size_t>(argv.Int64(2));
    result_double_or_null(ctx, statcpp::aicc(ll, n, k));
}
static void sf_bic(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double ll = argv.Double(0);
    auto n = static_cast<std::size_t>(argv.Int64(1));
    auto k = static_cast<std::size_t>(argv.Int64(2));
    result_double_or_null(ctx, statcpp::bic(ll, n, k));
}

// --- P6-40: Box-Cox transform ---
static void sf_boxcox(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double x = argv.Double(0);
    double lambda = argv.Double(1);
    // Box-Cox: (x^lambda - 1) / lambda if lambda != 0, else log(x)
    double result;
    if (std::abs(lambda) < 1e-10) {
        result = std::log(x);
    } else {
        result = (std::pow(x, lambda) - 1.0) / lambda;
    }
    result_double_or_null(ctx, result);
}

// ===========================================================================
// Scalar functions — distributions and transforms
// ===========================================================================

// --- P7-1..24: Continuous distributions (uniform, exponential, gamma, beta, lognormal, weibull) ---

static void sf_uniform_pdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double a = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double b = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::uniform_pdf(x, a, b));
}
static void sf_uniform_cdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double a = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double b = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::uniform_cdf(x, a, b));
}
static void sf_uniform_quantile(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double p = argv.Double(0);
    double a = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double b = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::uniform_quantile(p, a, b));
}
static void sf_uniform_rand(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double a = (argc > 0 && !argv.IsNull(0)) ? argv.Double(0) : 0.0;
    double b = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 1.0;
    result_double_or_null(ctx, statcpp::uniform_rand(a, b));
}

static void sf_exponential_pdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double lambda = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 1.0;
    result_double_or_null(ctx, statcpp::exponential_pdf(x, lambda));
}
static void sf_exponential_cdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double lambda = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 1.0;
    result_double_or_null(ctx, statcpp::exponential_cdf(x, lambda));
}
static void sf_exponential_quantile(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double p = argv.Double(0);
    double lambda = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 1.0;
    result_double_or_null(ctx, statcpp::exponential_quantile(p, lambda));
}
static void sf_exponential_rand(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double lambda = (argc > 0 && !argv.IsNull(0)) ? argv.Double(0) : 1.0;
    result_double_or_null(ctx, statcpp::exponential_rand(lambda));
}

static void sf_gamma_pdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::gamma_pdf(argv.Double(0),
                                                    argv.Double(1),
                                                    argv.Double(2)));
}
static void sf_gamma_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::gamma_cdf(argv.Double(0),
                                                    argv.Double(1),
                                                    argv.Double(2)));
}
static void sf_gamma_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::gamma_quantile(argv.Double(0),
                                                        argv.Double(1),
                                                        argv.Double(2)));
}
static void sf_gamma_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::gamma_rand(argv.Double(0),
                                                    argv.Double(1)));
}

static void sf_beta_pdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::beta_pdf(argv.Double(0),
                                                   argv.Double(1),
                                                   argv.Double(2)));
}
static void sf_beta_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::beta_cdf(argv.Double(0),
                                                   argv.Double(1),
                                                   argv.Double(2)));
}
static void sf_beta_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::beta_quantile(argv.Double(0),
                                                       argv.Double(1),
                                                       argv.Double(2)));
}
static void sf_beta_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::beta_rand(argv.Double(0),
                                                    argv.Double(1)));
}

static void sf_lognormal_pdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double mu = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double sigma = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::lognormal_pdf(x, mu, sigma));
}
static void sf_lognormal_cdf(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double x = argv.Double(0);
    double mu = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double sigma = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::lognormal_cdf(x, mu, sigma));
}
static void sf_lognormal_quantile(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double p = argv.Double(0);
    double mu = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.0;
    double sigma = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 1.0;
    result_double_or_null(ctx, statcpp::lognormal_quantile(p, mu, sigma));
}
static void sf_lognormal_rand(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double mu = (argc > 0 && !argv.IsNull(0)) ? argv.Double(0) : 0.0;
    double sigma = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 1.0;
    result_double_or_null(ctx, statcpp::lognormal_rand(mu, sigma));
}

static void sf_weibull_pdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::weibull_pdf(argv.Double(0),
                                                      argv.Double(1),
                                                      argv.Double(2)));
}
static void sf_weibull_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::weibull_cdf(argv.Double(0),
                                                      argv.Double(1),
                                                      argv.Double(2)));
}
static void sf_weibull_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::weibull_quantile(argv.Double(0),
                                                          argv.Double(1),
                                                          argv.Double(2)));
}
static void sf_weibull_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::weibull_rand(argv.Double(0),
                                                      argv.Double(1)));
}

// --- P7-25..52: Discrete distributions ---

static void sf_binomial_pmf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    auto n = static_cast<std::uint64_t>(argv.Int64(1));
    double p = argv.Double(2);
    result_double_or_null(ctx, statcpp::binomial_pmf(k, n, p));
}
static void sf_binomial_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    auto n = static_cast<std::uint64_t>(argv.Int64(1));
    double p = argv.Double(2);
    result_double_or_null(ctx, statcpp::binomial_cdf(k, n, p));
}
static void sf_binomial_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double prob = argv.Double(0);
    auto n = static_cast<std::uint64_t>(argv.Int64(1));
    double p = argv.Double(2);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::binomial_quantile(prob, n, p)));
}
static void sf_binomial_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto n = static_cast<std::uint64_t>(argv.Int64(0));
    double p = argv.Double(1);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::binomial_rand(n, p)));
}

static void sf_poisson_pmf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    double lambda = argv.Double(1);
    result_double_or_null(ctx, statcpp::poisson_pmf(k, lambda));
}
static void sf_poisson_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    double lambda = argv.Double(1);
    result_double_or_null(ctx, statcpp::poisson_cdf(k, lambda));
}
static void sf_poisson_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double p = argv.Double(0);
    double lambda = argv.Double(1);
    result_unbounded_quantile(ctx, statcpp::poisson_quantile(p, lambda));
}
static void sf_poisson_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double lambda = argv.Double(0);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::poisson_rand(lambda)));
}

static void sf_geometric_pmf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    double p = argv.Double(1);
    result_double_or_null(ctx, statcpp::geometric_pmf(k, p));
}
static void sf_geometric_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    double p = argv.Double(1);
    result_double_or_null(ctx, statcpp::geometric_cdf(k, p));
}
static void sf_geometric_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double prob = argv.Double(0);
    double p = argv.Double(1);
    result_unbounded_quantile(ctx, statcpp::geometric_quantile(prob, p));
}
static void sf_geometric_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double p = argv.Double(0);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::geometric_rand(p)));
}

static void sf_nbinom_pmf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    double r = argv.Double(1);
    double p = argv.Double(2);
    result_double_or_null(ctx, statcpp::nbinom_pmf(k, r, p));
}
static void sf_nbinom_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    double r = argv.Double(1);
    double p = argv.Double(2);
    result_double_or_null(ctx, statcpp::nbinom_cdf(k, r, p));
}
static void sf_nbinom_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double prob = argv.Double(0);
    double r = argv.Double(1);
    double p = argv.Double(2);
    result_unbounded_quantile(ctx, statcpp::nbinom_quantile(prob, r, p));
}
static void sf_nbinom_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double r = argv.Double(0);
    double p = argv.Double(1);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::nbinom_rand(r, p)));
}

static void sf_hypergeom_pmf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    auto N = static_cast<std::uint64_t>(argv.Int64(1));
    auto K = static_cast<std::uint64_t>(argv.Int64(2));
    auto n = static_cast<std::uint64_t>(argv.Int64(3));
    result_double_or_null(ctx, statcpp::hypergeom_pmf(k, N, K, n));
}
static void sf_hypergeom_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    auto N = static_cast<std::uint64_t>(argv.Int64(1));
    auto K = static_cast<std::uint64_t>(argv.Int64(2));
    auto n = static_cast<std::uint64_t>(argv.Int64(3));
    result_double_or_null(ctx, statcpp::hypergeom_cdf(k, N, K, n));
}
static void sf_hypergeom_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double p = argv.Double(0);
    auto N = static_cast<std::uint64_t>(argv.Int64(1));
    auto K = static_cast<std::uint64_t>(argv.Int64(2));
    auto n = static_cast<std::uint64_t>(argv.Int64(3));
    result_int64(ctx, static_cast<std::int64_t>(statcpp::hypergeom_quantile(p, N, K, n)));
}
static void sf_hypergeom_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto N = static_cast<std::uint64_t>(argv.Int64(0));
    auto K = static_cast<std::uint64_t>(argv.Int64(1));
    auto n = static_cast<std::uint64_t>(argv.Int64(2));
    result_int64(ctx, static_cast<std::int64_t>(statcpp::hypergeom_rand(N, K, n)));
}

static void sf_bernoulli_pmf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    double p = argv.Double(1);
    result_double_or_null(ctx, statcpp::bernoulli_pmf(k, p));
}
static void sf_bernoulli_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::uint64_t>(argv.Int64(0));
    double p = argv.Double(1);
    result_double_or_null(ctx, statcpp::bernoulli_cdf(k, p));
}
static void sf_bernoulli_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double prob = argv.Double(0);
    double p = argv.Double(1);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::bernoulli_quantile(prob, p)));
}
static void sf_bernoulli_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double p = argv.Double(0);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::bernoulli_rand(p)));
}

static void sf_duniform_pmf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::int64_t>(argv.Int64(0));
    auto a = static_cast<std::int64_t>(argv.Int64(1));
    auto b = static_cast<std::int64_t>(argv.Int64(2));
    result_double_or_null(ctx, statcpp::discrete_uniform_pmf(k, a, b));
}
static void sf_duniform_cdf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto k = static_cast<std::int64_t>(argv.Int64(0));
    auto a = static_cast<std::int64_t>(argv.Int64(1));
    auto b = static_cast<std::int64_t>(argv.Int64(2));
    result_double_or_null(ctx, statcpp::discrete_uniform_cdf(k, a, b));
}
static void sf_duniform_quantile(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double p = argv.Double(0);
    auto a = static_cast<std::int64_t>(argv.Int64(1));
    auto b = static_cast<std::int64_t>(argv.Int64(2));
    result_int64(ctx, static_cast<std::int64_t>(statcpp::discrete_uniform_quantile(p, a, b)));
}
static void sf_duniform_rand(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto a = static_cast<std::int64_t>(argv.Int64(0));
    auto b = static_cast<std::int64_t>(argv.Int64(1));
    result_int64(ctx, static_cast<std::int64_t>(statcpp::discrete_uniform_rand(a, b)));
}

// --- P7-53..55: Combinatorics ---
static void sf_binomial_coef(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto n = static_cast<std::uint64_t>(argv.Int64(0));
    auto k = static_cast<std::uint64_t>(argv.Int64(1));
    result_double_or_null(ctx, statcpp::binomial_coef(n, k));
}
static void sf_log_binomial_coef(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto n = static_cast<std::uint64_t>(argv.Int64(0));
    auto k = static_cast<std::uint64_t>(argv.Int64(1));
    result_double_or_null(ctx, statcpp::log_binomial_coef(n, k));
}
static void sf_log_factorial(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto n = static_cast<std::uint64_t>(argv.Int64(0));
    result_double_or_null(ctx, statcpp::log_factorial(n));
}

// --- P7-56..61: Special functions ---
static void sf_lgamma(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::lgamma(argv.Double(0)));
}
static void sf_tgamma(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::tgamma(argv.Double(0)));
}
static void sf_beta_func(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::beta(argv.Double(0),
                                              argv.Double(1)));
}
static void sf_lbeta(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::lbeta(argv.Double(0),
                                               argv.Double(1)));
}
static void sf_erf(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::erf(argv.Double(0)));
}
static void sf_erfc(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::erfc(argv.Double(0)));
}

// --- P7-62: Logarithmic mean ---
static void sf_logarithmic_mean(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double a = argv.Double(0);
    double b = argv.Double(1);
    double result;
    if (a <= 0.0 || b <= 0.0) { ctx.SetNull(); return; }
    if (std::abs(a - b) < 1e-15) { result = a; }
    else { result = (b - a) / (std::log(b) - std::log(a)); }
    result_double_or_null(ctx, result);
}

// --- P7-63: Hedges' correction factor J ---
static void sf_hedges_j(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double df = argv.Double(0);
    result_double_or_null(ctx, statcpp::hedges_correction_factor(df));
}

// --- P7-64..66: Effect size conversions ---
static void sf_t_to_r(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::t_to_r(argv.Double(0),
                                                argv.Double(1)));
}
static void sf_d_to_r(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::d_to_r(argv.Double(0)));
}
static void sf_r_to_d(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::r_to_d(argv.Double(0)));
}

// --- P7-67..69: Effect sizes from ANOVA components ---
static void sf_eta_squared_ef(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::eta_squared(argv.Double(0),
                                                     argv.Double(1)));
}
static void sf_partial_eta_sq(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::partial_eta_squared(argv.Double(0),
                                                             argv.Double(1),
                                                             argv.Double(2)));
}
static void sf_omega_squared_ef(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::omega_squared(argv.Double(0),
                                                       argv.Double(1),
                                                       argv.Double(2),
                                                       argv.Double(3)));
}

// --- P7-70: Cohen's h ---
static void sf_cohens_h(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    result_double_or_null(ctx, statcpp::cohens_h(argv.Double(0),
                                                  argv.Double(1)));
}

// --- P7-71..73: Effect size interpretation ---
static const char* magnitude_to_string(statcpp::effect_size_magnitude m) {
    switch (m) {
        case statcpp::effect_size_magnitude::negligible: return "negligible";
        case statcpp::effect_size_magnitude::small:      return "small";
        case statcpp::effect_size_magnitude::medium:     return "medium";
        case statcpp::effect_size_magnitude::large:      return "large";
    }
    return "unknown";
}

static void sf_interpret_d(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto m = statcpp::interpret_cohens_d(argv.Double(0));
    const char* s = magnitude_to_string(m);
    result_text(ctx, s);
}
static void sf_interpret_r(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto m = statcpp::interpret_correlation(argv.Double(0));
    const char* s = magnitude_to_string(m);
    result_text(ctx, s);
}
static void sf_interpret_eta2(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    auto m = statcpp::interpret_eta_squared(argv.Double(0));
    const char* s = magnitude_to_string(m);
    result_text(ctx, s);
}

// --- P7-74..79: Power analysis ---
static void sf_power_t1(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double es = argv.Double(0);
    auto n = static_cast<std::size_t>(argv.Int64(1));
    double alpha = argv.Double(2);
    result_double_or_null(ctx, statcpp::power_t_test_one_sample(es, n, alpha));
}
static void sf_n_t1(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double es = argv.Double(0);
    double power = argv.Double(1);
    double alpha = argv.Double(2);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::sample_size_t_test_one_sample(es, power, alpha)));
}
static void sf_power_t2(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double es = argv.Double(0);
    auto n1 = static_cast<std::size_t>(argv.Int64(1));
    auto n2 = static_cast<std::size_t>(argv.Int64(2));
    double alpha = argv.Double(3);
    result_double_or_null(ctx, statcpp::power_t_test_two_sample(es, n1, n2, alpha));
}
static void sf_n_t2(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double es = argv.Double(0);
    double power = argv.Double(1);
    double alpha = argv.Double(2);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::sample_size_t_test_two_sample(es, power, alpha)));
}
static void sf_power_prop(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double p1 = argv.Double(0);
    double p2 = argv.Double(1);
    auto n = static_cast<std::size_t>(argv.Int64(2));
    double alpha = argv.Double(3);
    result_double_or_null(ctx, statcpp::power_prop_test(p1, p2, n, alpha));
}
static void sf_n_prop(ResultSlot& ctx, int /*argc*/, const ArgRow& argv) {
    double p1 = argv.Double(0);
    double p2 = argv.Double(1);
    double power = argv.Double(2);
    double alpha = argv.Double(3);
    result_int64(ctx, static_cast<std::int64_t>(statcpp::sample_size_prop_test(p1, p2, power, alpha)));
}

// --- P7-80..83: Margin of error / sample size estimation ---
static void sf_moe_prop(ResultSlot& ctx, int argc, const ArgRow& argv) {
    auto succ = static_cast<std::size_t>(argv.Int64(0));
    auto n = static_cast<std::size_t>(argv.Int64(1));
    double conf = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 0.95;
    result_double_or_null(ctx, statcpp::margin_of_error_proportion(succ, n, conf));
}
static void sf_moe_prop_worst(ResultSlot& ctx, int argc, const ArgRow& argv) {
    auto n = static_cast<std::size_t>(argv.Int64(0));
    double conf = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.95;
    result_double_or_null(ctx, statcpp::margin_of_error_proportion_worst_case(n, conf));
}
static void sf_n_moe_prop(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double moe = argv.Double(0);
    double conf = (argc > 1 && !argv.IsNull(1)) ? argv.Double(1) : 0.95;
    double p_est = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 0.5;
    result_int64(ctx, static_cast<std::int64_t>(statcpp::sample_size_for_moe_proportion(moe, conf, p_est)));
}
static void sf_n_moe_mean(ResultSlot& ctx, int argc, const ArgRow& argv) {
    double moe = argv.Double(0);
    double sigma = argv.Double(1);
    double conf = (argc > 2 && !argv.IsNull(2)) ? argv.Double(2) : 0.95;
    result_int64(ctx, static_cast<std::int64_t>(statcpp::sample_size_for_moe_mean(moe, sigma, conf)));
}


// ===========================================================================
// Register all functions
// ===========================================================================

/**
 * @brief Register every SQL function of the extension
 * @param loader Extension loader
 */
static void register_all(ExtensionLoader& loader) {
    // --- Basic aggregates: SingleColumnAggregate (24 functions) ---

    // Basic statistics
    SingleColumnAggregate<calc_mean>::register_func(loader, "stat_mean");
    SingleColumnAggregate<calc_median>::register_func(loader, "stat_median");
    SingleColumnAggregate<calc_mode>::register_func(loader, "stat_mode");
    SingleColumnAggregate<calc_geometric_mean>::register_func(loader, "stat_geometric_mean");
    SingleColumnAggregate<calc_harmonic_mean>::register_func(loader, "stat_harmonic_mean");

    // Dispersion / spread
    SingleColumnAggregate<calc_range>::register_func(loader, "stat_range");
    SingleColumnAggregate<calc_var>::register_func(loader, "stat_var");
    SingleColumnAggregate<calc_population_variance>::register_func(loader, "stat_population_variance");
    SingleColumnAggregate<calc_sample_variance>::register_func(loader, "stat_sample_variance");
    SingleColumnAggregate<calc_stdev>::register_func(loader, "stat_stdev");
    SingleColumnAggregate<calc_population_stddev>::register_func(loader, "stat_population_stddev");
    SingleColumnAggregate<calc_sample_stddev>::register_func(loader, "stat_sample_stddev");
    SingleColumnAggregate<calc_cv>::register_func(loader, "stat_cv");
    SingleColumnAggregate<calc_iqr>::register_func(loader, "stat_iqr");
    SingleColumnAggregate<calc_mad_mean>::register_func(loader, "stat_mad_mean");
    SingleColumnAggregate<calc_geometric_stddev>::register_func(loader, "stat_geometric_stddev");

    // Shape of distribution
    SingleColumnAggregate<calc_population_skewness>::register_func(loader, "stat_population_skewness");
    SingleColumnAggregate<calc_skewness>::register_func(loader, "stat_skewness");
    SingleColumnAggregate<calc_population_kurtosis>::register_func(loader, "stat_population_kurtosis");
    SingleColumnAggregate<calc_kurtosis>::register_func(loader, "stat_kurtosis");

    // Estimation
    SingleColumnAggregate<calc_se>::register_func(loader, "stat_se");

    // Robust statistics
    SingleColumnAggregate<calc_mad>::register_func(loader, "stat_mad");
    SingleColumnAggregate<calc_mad_scaled>::register_func(loader, "stat_mad_scaled");
    SingleColumnAggregate<calc_hodges_lehmann>::register_func(loader, "stat_hodges_lehmann");

    // --- Parameterized aggregates: SingleColumnParamAggregate (20 functions) ---

    // Basic statistics (parameterized)
    SingleColumnParamAggregate<1, calc_trimmed_mean>::register_func(loader, "stat_trimmed_mean");

    // Order statistics (parameterized)
    SingleColumnParamAggregateText<0, calc_quartile>::register_func(loader, "stat_quartile");
    SingleColumnParamAggregate<1, calc_percentile>::register_func(loader, "stat_percentile");

    // Parametric tests
    SingleColumnParamAggregateText<2, calc_z_test>::register_func(loader, "stat_z_test");
    SingleColumnParamAggregateText<1, calc_t_test>::register_func(loader, "stat_t_test");
    SingleColumnParamAggregateText<0, calc_chisq_gof_uniform>::register_func(loader, "stat_chisq_gof_uniform");

    // Nonparametric tests
    SingleColumnParamAggregateText<0, calc_shapiro_wilk>::register_func(loader, "stat_shapiro_wilk");
    SingleColumnParamAggregateText<0, calc_ks_test>::register_func(loader, "stat_ks_test");
    SingleColumnParamAggregateText<1, calc_wilcoxon>::register_func(loader, "stat_wilcoxon");

    // Estimation
    SingleColumnParamAggregateText<1, calc_ci_mean>::register_func(loader, "stat_ci_mean");
    SingleColumnParamAggregateText<2, calc_ci_mean_z>::register_func(loader, "stat_ci_mean_z");
    SingleColumnParamAggregateText<1, calc_ci_var>::register_func(loader, "stat_ci_var");
    SingleColumnParamAggregate<1, calc_moe_mean>::register_func(loader, "stat_moe_mean");

    // Effect size
    SingleColumnParamAggregate<1, calc_cohens_d>::register_func(loader, "stat_cohens_d");
    SingleColumnParamAggregate<1, calc_hedges_g>::register_func(loader, "stat_hedges_g");

    // Time series
    SingleColumnParamAggregate<1, calc_acf_lag>::register_func(loader, "stat_acf_lag");

    // Robust statistics (parameterized)
    SingleColumnParamAggregate<1, calc_biweight_midvar>::register_func(loader, "stat_biweight_midvar");

    // Resampling
    SingleColumnParamAggregateText<1, calc_bootstrap_mean>::register_func(loader, "stat_bootstrap_mean");
    SingleColumnParamAggregateText<1, calc_bootstrap_median>::register_func(loader, "stat_bootstrap_median");
    SingleColumnParamAggregateText<1, calc_bootstrap_stddev>::register_func(loader, "stat_bootstrap_stddev");

    // --- Two-column aggregates: TwoColumnAggregate (27 functions) ---

    // Correlation / covariance
    TwoColumnAggregate<calc_population_covariance>::register_func(loader, "stat_population_covariance");
    TwoColumnAggregate<calc_covariance>::register_func(loader, "stat_covariance");
    TwoColumnAggregate<calc_pearson_r>::register_func(loader, "stat_pearson_r");
    TwoColumnAggregate<calc_spearman_r>::register_func(loader, "stat_spearman_r");
    TwoColumnAggregate<calc_kendall_tau>::register_func(loader, "stat_kendall_tau");
    TwoColumnAggregate<calc_weighted_covariance>::register_func(loader, "stat_weighted_covariance");

    // Weighted statistics
    TwoColumnAggregate<calc_weighted_mean>::register_func(loader, "stat_weighted_mean");
    TwoColumnAggregate<calc_weighted_harmonic_mean>::register_func(loader, "stat_weighted_harmonic_mean");
    TwoColumnAggregate<calc_weighted_variance>::register_func(loader, "stat_weighted_variance");
    TwoColumnAggregate<calc_weighted_stddev>::register_func(loader, "stat_weighted_stddev");
    TwoColumnAggregate<calc_weighted_median>::register_func(loader, "stat_weighted_median");
    TwoColumnParamAggregate<1, calc_weighted_percentile>::register_func(loader, "stat_weighted_percentile");

    // Linear regression
    TwoColumnAggregateText<calc_simple_regression>::register_func(loader, "stat_simple_regression");
    TwoColumnAggregate<calc_r_squared>::register_func(loader, "stat_r_squared");
    TwoColumnAggregate<calc_adjusted_r_squared>::register_func(loader, "stat_adjusted_r_squared");

    // Paired / two-sample tests
    TwoColumnAggregateText<calc_t_test_paired>::register_func(loader, "stat_t_test_paired");
    TwoColumnAggregateText<calc_chisq_gof>::register_func(loader, "stat_chisq_gof");

    // Error metrics
    TwoColumnAggregate<calc_mae>::register_func(loader, "stat_mae");
    TwoColumnAggregate<calc_mse>::register_func(loader, "stat_mse");
    TwoColumnAggregate<calc_rmse>::register_func(loader, "stat_rmse");
    TwoColumnAggregate<calc_mape>::register_func(loader, "stat_mape");

    // Distance metrics
    TwoColumnAggregate<calc_euclidean_dist>::register_func(loader, "stat_euclidean_dist");
    TwoColumnAggregate<calc_manhattan_dist>::register_func(loader, "stat_manhattan_dist");
    TwoColumnAggregate<calc_cosine_sim>::register_func(loader, "stat_cosine_sim");
    TwoColumnAggregate<calc_cosine_dist>::register_func(loader, "stat_cosine_dist");
    TwoColumnParamAggregate<1, calc_minkowski_dist>::register_func(loader, "stat_minkowski_dist");
    TwoColumnAggregate<calc_chebyshev_dist>::register_func(loader, "stat_chebyshev_dist");

    // --- Window functions: FullScanWindowFunction (23 functions) ---

    // Rolling statistics (2-arg: column, window_size)
    FullScanWindowFunction<wf_rolling_mean>::register_func_2(loader, "stat_rolling_mean");
    FullScanWindowFunction<wf_rolling_std>::register_func_2(loader, "stat_rolling_std");
    FullScanWindowFunction<wf_rolling_min>::register_func_2(loader, "stat_rolling_min");
    FullScanWindowFunction<wf_rolling_max>::register_func_2(loader, "stat_rolling_max");
    FullScanWindowFunction<wf_rolling_sum>::register_func_2(loader, "stat_rolling_sum");

    // Moving averages (2-arg: column, window/span)
    FullScanWindowFunction<wf_moving_avg>::register_func_2(loader, "stat_moving_avg");
    FullScanWindowFunction<wf_ema>::register_func_2(loader, "stat_ema");

    // Rank (1-arg: column)
    FullScanWindowFunction<wf_rank>::register_func_1(loader, "stat_rank");

    // Fill NA (1-arg: column)
    FullScanWindowFunction<wf_fillna_mean>::register_func_1(loader, "stat_fillna_mean");
    FullScanWindowFunction<wf_fillna_median>::register_func_1(loader, "stat_fillna_median");
    FullScanWindowFunction<wf_fillna_ffill>::register_func_1(loader, "stat_fillna_ffill");
    FullScanWindowFunction<wf_fillna_bfill>::register_func_1(loader, "stat_fillna_bfill");
    FullScanWindowFunction<wf_fillna_interp>::register_func_1(loader, "stat_fillna_interp");

    // Encoding (1-arg: column or 2-arg: column, n_bins)
    FullScanWindowFunction<wf_label_encode>::register_func_1(loader, "stat_label_encode");
    FullScanWindowFunction<wf_bin_width>::register_func_2(loader, "stat_bin_width");
    FullScanWindowFunction<wf_bin_freq>::register_func_2(loader, "stat_bin_freq");

    // Time series (2-arg: column, lag/order/period)
    FullScanWindowFunction<wf_lag>::register_func_2(loader, "stat_lag");
    FullScanWindowFunction<wf_diff>::register_func_2(loader, "stat_diff");
    FullScanWindowFunction<wf_seasonal_diff>::register_func_2(loader, "stat_seasonal_diff");

    // Outlier detection (1-arg: column)
    FullScanWindowFunction<wf_outliers_iqr>::register_func_1(loader, "stat_outliers_iqr");
    FullScanWindowFunction<wf_outliers_zscore>::register_func_1(loader, "stat_outliers_zscore");
    FullScanWindowFunction<wf_outliers_mzscore>::register_func_1(loader, "stat_outliers_mzscore");

    // Robust (2-arg: column, percentile)
    FullScanWindowFunction<wf_winsorize>::register_func_2(loader, "stat_winsorize");

    // --- Multiple testing corrections (window: corrects a whole set of p-values) ---
    FullScanWindowFunction<wf_bonferroni>::register_func_1(loader, "stat_bonferroni");
    FullScanWindowFunction<wf_bh_correction>::register_func_1(loader, "stat_bh_correction");
    FullScanWindowFunction<wf_holm_correction>::register_func_1(loader, "stat_holm_correction");

    // --- Complex aggregates: two-sample / JSON (32 functions) ---

    // Multiple result aggregates (single column → JSON)
    SingleColumnAggregateText<calc_modes>::register_func(loader, "stat_modes");
    SingleColumnAggregateText<calc_five_number_summary>::register_func(loader, "stat_five_number_summary");
    SingleColumnAggregateText<calc_frequency_table>::register_func(loader, "stat_frequency_table");
    SingleColumnAggregateText<calc_frequency_count>::register_func(loader, "stat_frequency_count");
    SingleColumnAggregateText<calc_relative_frequency>::register_func(loader, "stat_relative_frequency");
    SingleColumnAggregateText<calc_cumulative_frequency>::register_func(loader, "stat_cumulative_frequency");
    SingleColumnAggregateText<calc_cumulative_relative_frequency>::register_func(loader, "stat_cumulative_relative_frequency");

    // Two-sample tests (two-column → JSON)
    TwoColumnAggregateText<calc_t_test2>::register_func(loader, "stat_t_test2");
    TwoColumnAggregateText<calc_t_test_welch>::register_func(loader, "stat_t_test_welch");
    TwoColumnAggregateText<calc_chisq_independence>::register_func(loader, "stat_chisq_independence");
    TwoColumnAggregateText<calc_f_test>::register_func(loader, "stat_f_test");
    TwoColumnAggregateText<calc_mann_whitney>::register_func(loader, "stat_mann_whitney");

    // ANOVA (two-column: value, group → JSON)
    TwoColumnAggregateText<calc_anova1>::register_func(loader, "stat_anova1");

    // --- Group-column form: tests and post-hoc tests (9 functions) ---
    TwoColumnAggregateText<calc_kruskal_wallis>::register_func(loader, "stat_kruskal_wallis");
    TwoColumnAggregateText<calc_levene>::register_func(loader, "stat_levene");
    TwoColumnAggregateText<calc_bartlett>::register_func(loader, "stat_bartlett");
    TwoColumnAggregate<calc_cohens_f>::register_func(loader, "stat_cohens_f");
    TwoColumnParamAggregateText<1, calc_tukey_hsd>::register_func(loader, "stat_tukey_hsd");
    TwoColumnParamAggregateText<1, calc_bonferroni_posthoc>::register_func(loader, "stat_bonferroni_posthoc");
    TwoColumnParamAggregateText<1, calc_scheffe_posthoc>::register_func(loader, "stat_scheffe_posthoc");
    TwoColumnParamAggregateText<2, calc_dunnett_posthoc>::register_func(loader, "stat_dunnett_posthoc");
    TwoColumnParamAggregateText<1, calc_stratified_sample>::register_func(loader, "stat_stratified_sample");

    // Contingency table (two-column → JSON)
    TwoColumnAggregateText<calc_contingency_table>::register_func(loader, "stat_contingency_table");

    // Two-sample effect sizes
    TwoColumnAggregate<calc_cohens_d2>::register_func(loader, "stat_cohens_d2");
    TwoColumnAggregate<calc_hedges_g2>::register_func(loader, "stat_hedges_g2");
    TwoColumnAggregate<calc_glass_delta>::register_func(loader, "stat_glass_delta");

    // CI for differences (two-column → JSON)
    TwoColumnAggregateText<calc_ci_mean_diff>::register_func(loader, "stat_ci_mean_diff");
    TwoColumnAggregateText<calc_ci_mean_diff_welch>::register_func(loader, "stat_ci_mean_diff_welch");

    // Survival analysis (two-column: time, event → JSON)
    TwoColumnAggregateText<calc_kaplan_meier>::register_func(loader, "stat_kaplan_meier");
    // Logrank test (three-column: time, event, group → JSON)
    LogrankAggregate::register_func(loader, "stat_logrank");
    TwoColumnAggregateText<calc_nelson_aalen>::register_func(loader, "stat_nelson_aalen");

    // Resampling (single column + param → JSON)
    SingleColumnParamAggregateText<1, calc_bootstrap_generic>::register_func(loader, "stat_bootstrap");
    SingleColumnParamAggregateText<1, calc_bootstrap_bca>::register_func(loader, "stat_bootstrap_bca");
    SingleColumnParamAggregateText<0, calc_bootstrap_sample_text>::register_func(loader, "stat_bootstrap_sample");

    // Permutation tests (two-column → JSON)
    TwoColumnAggregateText<calc_permutation_test2>::register_func(loader, "stat_permutation_test2");
    TwoColumnAggregateText<calc_permutation_paired>::register_func(loader, "stat_permutation_paired");
    TwoColumnAggregateText<calc_permutation_corr>::register_func(loader, "stat_permutation_corr");

    // ACF / PACF (single column + max_lag → JSON array)
    SingleColumnParamAggregateText<1, calc_acf>::register_func(loader, "stat_acf");
    SingleColumnParamAggregateText<1, calc_pacf>::register_func(loader, "stat_pacf");

    // Sampling (single column + n → JSON array)
    SingleColumnParamAggregateText<1, calc_sample_replace>::register_func(loader, "stat_sample_replace");
    SingleColumnParamAggregateText<1, calc_sample_noreplace>::register_func(loader, "stat_sample");

    // --- Scalar — tests/helpers (40 functions) ---

    // Normal distribution (1-3 args)
    register_scalar<sf_normal_pdf>(loader, "stat_normal_pdf", {1, 2, 3});
    register_scalar<sf_normal_cdf>(loader, "stat_normal_cdf", {1, 2, 3});
    register_scalar<sf_normal_quantile>(loader, "stat_normal_quantile", {1, 2, 3});
    register_scalar_nd<sf_normal_rand>(loader, "stat_normal_rand", {0, 1, 2});

    // Chi-square distribution (2 args)
    register_scalar<sf_chisq_pdf>(loader, "stat_chisq_pdf", {2});
    register_scalar<sf_chisq_cdf>(loader, "stat_chisq_cdf", {2});
    register_scalar<sf_chisq_quantile>(loader, "stat_chisq_quantile", {2});
    register_scalar_nd<sf_chisq_rand>(loader, "stat_chisq_rand", {1});

    // T distribution (2 args)
    register_scalar<sf_t_pdf>(loader, "stat_t_pdf", {2});
    register_scalar<sf_t_cdf>(loader, "stat_t_cdf", {2});
    register_scalar<sf_t_quantile>(loader, "stat_t_quantile", {2});
    register_scalar_nd<sf_t_rand>(loader, "stat_t_rand", {1});

    // F distribution (3 args)
    register_scalar<sf_f_pdf>(loader, "stat_f_pdf", {3});
    register_scalar<sf_f_cdf>(loader, "stat_f_cdf", {3});
    register_scalar<sf_f_quantile>(loader, "stat_f_quantile", {3});
    register_scalar_nd<sf_f_rand>(loader, "stat_f_rand", {2});

    // Special functions
    register_scalar<sf_betainc>(loader, "stat_betainc", {3});
    register_scalar<sf_betaincinv>(loader, "stat_betaincinv", {3});
    register_scalar<sf_norm_cdf>(loader, "stat_norm_cdf", {1});
    register_scalar<sf_norm_quantile>(loader, "stat_norm_quantile", {1});
    register_scalar<sf_gammainc_lower>(loader, "stat_gammainc_lower", {2});
    register_scalar<sf_gammainc_upper>(loader, "stat_gammainc_upper", {2});
    register_scalar<sf_gammainc_lower_inv>(loader, "stat_gammainc_lower_inv", {2});

    // Proportion tests
    register_scalar<sf_z_test_prop>(loader, "stat_z_test_prop", {3}, LogicalType::VARCHAR);
    register_scalar<sf_z_test_prop2>(loader, "stat_z_test_prop2", {4}, LogicalType::VARCHAR);

    // Multiple testing corrections
    register_scalar<sf_bonferroni>(loader, "stat_bonferroni_scalar", {2});

    // Categorical tests
    register_scalar<sf_fisher_exact>(loader, "stat_fisher_exact", {4}, LogicalType::VARCHAR);
    register_scalar<sf_odds_ratio>(loader, "stat_odds_ratio", {4}, LogicalType::VARCHAR);
    register_scalar<sf_relative_risk>(loader, "stat_relative_risk", {4}, LogicalType::VARCHAR);
    register_scalar<sf_risk_difference>(loader, "stat_risk_difference", {4}, LogicalType::VARCHAR);
    register_scalar<sf_nnt>(loader, "stat_nnt", {4});

    // Proportion CIs
    register_scalar<sf_ci_prop>(loader, "stat_ci_prop", {2, 3}, LogicalType::VARCHAR);
    register_scalar<sf_ci_prop_wilson>(loader, "stat_ci_prop_wilson", {2, 3}, LogicalType::VARCHAR);
    register_scalar<sf_ci_prop_diff>(loader, "stat_ci_prop_diff", {4, 5}, LogicalType::VARCHAR);

    // Model selection
    register_scalar<sf_aic>(loader, "stat_aic", {2});
    register_scalar<sf_aicc>(loader, "stat_aicc", {3});
    register_scalar<sf_bic>(loader, "stat_bic", {3});

    // Data transformation
    register_scalar<sf_boxcox>(loader, "stat_boxcox", {2});

    // --- Scalar — distributions/transforms (83 functions) ---

    // Uniform distribution
    register_scalar<sf_uniform_pdf>(loader, "stat_uniform_pdf", {1, 2, 3});
    register_scalar<sf_uniform_cdf>(loader, "stat_uniform_cdf", {1, 2, 3});
    register_scalar<sf_uniform_quantile>(loader, "stat_uniform_quantile", {1, 2, 3});
    register_scalar_nd<sf_uniform_rand>(loader, "stat_uniform_rand", {0, 1, 2});

    // Exponential distribution
    register_scalar<sf_exponential_pdf>(loader, "stat_exponential_pdf", {1, 2});
    register_scalar<sf_exponential_cdf>(loader, "stat_exponential_cdf", {1, 2});
    register_scalar<sf_exponential_quantile>(loader, "stat_exponential_quantile", {1, 2});
    register_scalar_nd<sf_exponential_rand>(loader, "stat_exponential_rand", {0, 1});

    // Gamma distribution
    register_scalar<sf_gamma_pdf>(loader, "stat_gamma_pdf", {3});
    register_scalar<sf_gamma_cdf>(loader, "stat_gamma_cdf", {3});
    register_scalar<sf_gamma_quantile>(loader, "stat_gamma_quantile", {3});
    register_scalar_nd<sf_gamma_rand>(loader, "stat_gamma_rand", {2});

    // Beta distribution
    register_scalar<sf_beta_pdf>(loader, "stat_beta_pdf", {3});
    register_scalar<sf_beta_cdf>(loader, "stat_beta_cdf", {3});
    register_scalar<sf_beta_quantile>(loader, "stat_beta_quantile", {3});
    register_scalar_nd<sf_beta_rand>(loader, "stat_beta_rand", {2});

    // Log-normal distribution
    register_scalar<sf_lognormal_pdf>(loader, "stat_lognormal_pdf", {1, 2, 3});
    register_scalar<sf_lognormal_cdf>(loader, "stat_lognormal_cdf", {1, 2, 3});
    register_scalar<sf_lognormal_quantile>(loader, "stat_lognormal_quantile", {1, 2, 3});
    register_scalar_nd<sf_lognormal_rand>(loader, "stat_lognormal_rand", {0, 1, 2});

    // Weibull distribution
    register_scalar<sf_weibull_pdf>(loader, "stat_weibull_pdf", {3});
    register_scalar<sf_weibull_cdf>(loader, "stat_weibull_cdf", {3});
    register_scalar<sf_weibull_quantile>(loader, "stat_weibull_quantile", {3});
    register_scalar_nd<sf_weibull_rand>(loader, "stat_weibull_rand", {2});

    // Binomial distribution
    register_scalar<sf_binomial_pmf>(loader, "stat_binomial_pmf", {3});
    register_scalar<sf_binomial_cdf>(loader, "stat_binomial_cdf", {3});
    register_scalar<sf_binomial_quantile>(loader, "stat_binomial_quantile", {3}, LogicalType::BIGINT);
    register_scalar_nd<sf_binomial_rand>(loader, "stat_binomial_rand", {2}, LogicalType::BIGINT);

    // Poisson distribution
    register_scalar<sf_poisson_pmf>(loader, "stat_poisson_pmf", {2});
    register_scalar<sf_poisson_cdf>(loader, "stat_poisson_cdf", {2});
    register_scalar<sf_poisson_quantile>(loader, "stat_poisson_quantile", {2}, LogicalType::BIGINT);
    register_scalar_nd<sf_poisson_rand>(loader, "stat_poisson_rand", {1}, LogicalType::BIGINT);

    // Geometric distribution
    register_scalar<sf_geometric_pmf>(loader, "stat_geometric_pmf", {2});
    register_scalar<sf_geometric_cdf>(loader, "stat_geometric_cdf", {2});
    register_scalar<sf_geometric_quantile>(loader, "stat_geometric_quantile", {2}, LogicalType::BIGINT);
    register_scalar_nd<sf_geometric_rand>(loader, "stat_geometric_rand", {1}, LogicalType::BIGINT);

    // Negative binomial distribution
    register_scalar<sf_nbinom_pmf>(loader, "stat_nbinom_pmf", {3});
    register_scalar<sf_nbinom_cdf>(loader, "stat_nbinom_cdf", {3});
    register_scalar<sf_nbinom_quantile>(loader, "stat_nbinom_quantile", {3}, LogicalType::BIGINT);
    register_scalar_nd<sf_nbinom_rand>(loader, "stat_nbinom_rand", {2}, LogicalType::BIGINT);

    // Hypergeometric distribution
    register_scalar<sf_hypergeom_pmf>(loader, "stat_hypergeom_pmf", {4});
    register_scalar<sf_hypergeom_cdf>(loader, "stat_hypergeom_cdf", {4});
    register_scalar<sf_hypergeom_quantile>(loader, "stat_hypergeom_quantile", {4}, LogicalType::BIGINT);
    register_scalar_nd<sf_hypergeom_rand>(loader, "stat_hypergeom_rand", {3}, LogicalType::BIGINT);

    // Bernoulli distribution
    register_scalar<sf_bernoulli_pmf>(loader, "stat_bernoulli_pmf", {2});
    register_scalar<sf_bernoulli_cdf>(loader, "stat_bernoulli_cdf", {2});
    register_scalar<sf_bernoulli_quantile>(loader, "stat_bernoulli_quantile", {2}, LogicalType::BIGINT);
    register_scalar_nd<sf_bernoulli_rand>(loader, "stat_bernoulli_rand", {1}, LogicalType::BIGINT);

    // Discrete uniform distribution
    register_scalar<sf_duniform_pmf>(loader, "stat_duniform_pmf", {3});
    register_scalar<sf_duniform_cdf>(loader, "stat_duniform_cdf", {3});
    register_scalar<sf_duniform_quantile>(loader, "stat_duniform_quantile", {3}, LogicalType::BIGINT);
    register_scalar_nd<sf_duniform_rand>(loader, "stat_duniform_rand", {2}, LogicalType::BIGINT);

    // Combinatorics
    register_scalar<sf_binomial_coef>(loader, "stat_binomial_coef", {2});
    register_scalar<sf_log_binomial_coef>(loader, "stat_log_binomial_coef", {2});
    register_scalar<sf_log_factorial>(loader, "stat_log_factorial", {1});

    // Special functions
    register_scalar<sf_lgamma>(loader, "stat_lgamma", {1});
    register_scalar<sf_tgamma>(loader, "stat_tgamma", {1});
    register_scalar<sf_beta_func>(loader, "stat_beta_func", {2});
    register_scalar<sf_lbeta>(loader, "stat_lbeta", {2});
    register_scalar<sf_erf>(loader, "stat_erf", {1});
    register_scalar<sf_erfc>(loader, "stat_erfc", {1});

    // Basic statistics (scalar)
    register_scalar<sf_logarithmic_mean>(loader, "stat_logarithmic_mean", {2});

    // Effect size corrections/conversions
    register_scalar<sf_hedges_j>(loader, "stat_hedges_j", {1});
    register_scalar<sf_t_to_r>(loader, "stat_t_to_r", {2});
    register_scalar<sf_d_to_r>(loader, "stat_d_to_r", {1});
    register_scalar<sf_r_to_d>(loader, "stat_r_to_d", {1});
    register_scalar<sf_eta_squared_ef>(loader, "stat_eta_squared_ef", {2});
    register_scalar<sf_partial_eta_sq>(loader, "stat_partial_eta_sq", {3});
    register_scalar<sf_omega_squared_ef>(loader, "stat_omega_squared_ef", {4});
    register_scalar<sf_cohens_h>(loader, "stat_cohens_h", {2});

    // Effect size interpretation
    register_scalar<sf_interpret_d>(loader, "stat_interpret_d", {1}, LogicalType::VARCHAR);
    register_scalar<sf_interpret_r>(loader, "stat_interpret_r", {1}, LogicalType::VARCHAR);
    register_scalar<sf_interpret_eta2>(loader, "stat_interpret_eta2", {1}, LogicalType::VARCHAR);

    // Power analysis
    register_scalar<sf_power_t1>(loader, "stat_power_t1", {3});
    register_scalar<sf_n_t1>(loader, "stat_n_t1", {3}, LogicalType::BIGINT);
    register_scalar<sf_power_t2>(loader, "stat_power_t2", {4});
    register_scalar<sf_n_t2>(loader, "stat_n_t2", {3}, LogicalType::BIGINT);
    register_scalar<sf_power_prop>(loader, "stat_power_prop", {4});
    register_scalar<sf_n_prop>(loader, "stat_n_prop", {4}, LogicalType::BIGINT);

    // Margin of error / sample size
    register_scalar<sf_moe_prop>(loader, "stat_moe_prop", {2, 3});
    register_scalar<sf_moe_prop_worst>(loader, "stat_moe_prop_worst", {1, 2});
    register_scalar<sf_n_moe_prop>(loader, "stat_n_moe_prop", {1, 2, 3}, LogicalType::BIGINT);
    register_scalar<sf_n_moe_mean>(loader, "stat_n_moe_mean", {2, 3}, LogicalType::BIGINT);
}

}  // namespace duckdb_stats

// ===========================================================================
// Entry point
//
// DuckDB looks up <extension name>_duckdb_cpp_init, so the symbol must not be
// C++-mangled: the macro is wrapped in extern "C".
// ===========================================================================

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ext_funcs, loader) {
    duckdb_stats::register_all(loader);
}

}
