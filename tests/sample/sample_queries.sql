-- Sample queries: representative statistical functions on the students table
--
-- Usage (from the project root, with the extension built). The official duckdb
-- CLI refuses to load an extension by a relative path, so pass an absolute one:
--
--   duckdb -unsigned \
--       -cmd "LOAD '$PWD/build/ext_funcs.duckdb_extension'" \
--       -c ".read tests/sample/sample_data.sql" \
--       -c ".read tests/sample/sample_queries.sql"

-- ---------------------------------------------------------------------------
-- 1. Descriptive statistics: what do the scores look like?
-- ---------------------------------------------------------------------------
SELECT stat_mean(score)                      AS mean,
       stat_median(score)                    AS median,
       round(stat_sample_stddev(score), 2)   AS sd,
       stat_percentile(score, 0.25)          AS q1,
       stat_percentile(score, 0.75)          AS q3
FROM students;

-- ---------------------------------------------------------------------------
-- 2. Aggregates work with GROUP BY: the same statistics per class
-- ---------------------------------------------------------------------------
SELECT class,
       stat_mean(score)                      AS mean,
       stat_median(score)                    AS median,
       round(stat_sample_stddev(score), 2)   AS sd
FROM students
GROUP BY class
ORDER BY class;

-- ---------------------------------------------------------------------------
-- 3. Two-sample test: is the difference between the classes significant?
--    The two-sample functions take the groups as two columns, so the classes
--    are placed side by side (50 students each). p_value < 0.05 means the
--    difference is unlikely to be due to chance.
-- ---------------------------------------------------------------------------
WITH a AS (SELECT score, row_number() OVER (ORDER BY id) AS n FROM students WHERE class = 'A'),
     b AS (SELECT score, row_number() OVER (ORDER BY id) AS n FROM students WHERE class = 'B')
SELECT stat_t_test_welch(a.score, b.score)            AS welch_t_test,
       round(stat_cohens_d2(a.score, b.score), 3)     AS cohens_d
FROM a JOIN b USING (n);

-- ---------------------------------------------------------------------------
-- 4. Correlation and regression: how much does one hour of study add?
-- ---------------------------------------------------------------------------
SELECT round(stat_pearson_r(study_hours, score), 3)                          AS pearson_r,
       round(json_extract(stat_simple_regression(study_hours, score), '$.slope')::DOUBLE, 2)
                                                                             AS points_per_hour,
       round(json_extract(stat_simple_regression(study_hours, score), '$.intercept')::DOUBLE, 2)
                                                                             AS intercept,
       round(json_extract(stat_simple_regression(study_hours, score), '$.r_squared')::DOUBLE, 3)
                                                                             AS r_squared
FROM students;

-- ---------------------------------------------------------------------------
-- 5. Window functions: rank every student and flag outliers (IQR rule).
--    Window functions need an OVER (ORDER BY ...) clause.
-- ---------------------------------------------------------------------------
SELECT id, class, study_hours, score,
       stat_rank(score) OVER (ORDER BY id)          AS rank_low_to_high,
       stat_outliers_iqr(score) OVER (ORDER BY id)  AS is_outlier
FROM students
QUALIFY is_outlier = 1 OR rank_low_to_high >= 98
ORDER BY score;

-- ---------------------------------------------------------------------------
-- 6. Combining results: students far from the regression line.
--    Student 17 has an ordinary-looking high score, so the IQR rule above
--    misses it, but it is unusual for only 1.2 hours of study.
-- ---------------------------------------------------------------------------
WITH fit AS (
    SELECT json_extract(stat_simple_regression(study_hours, score), '$.slope')::DOUBLE     AS slope,
           json_extract(stat_simple_regression(study_hours, score), '$.intercept')::DOUBLE AS intercept
    FROM students
)
SELECT id, study_hours, score,
       round(intercept + slope * study_hours, 1)          AS predicted,
       round(score - (intercept + slope * study_hours), 1) AS residual
FROM students, fit
ORDER BY abs(residual) DESC
LIMIT 3;

-- ---------------------------------------------------------------------------
-- 7. Scalar distribution functions: what share of students score above 80,
--    if scores followed a normal distribution with this mean and SD?
-- ---------------------------------------------------------------------------
SELECT round(1 - stat_normal_cdf(80, stat_mean(score), stat_sample_stddev(score)), 3) AS expected_share,
       round(avg(CASE WHEN score > 80 THEN 1 ELSE 0 END), 3)                           AS observed_share
FROM students;
