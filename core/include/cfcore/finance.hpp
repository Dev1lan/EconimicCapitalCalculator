#pragma once

#include <cmath>
#include <optional>
#include <span>

#include "cfcore/deal.hpp"

namespace cfcore {

// Доля года между датами. Если to <= from, результат 0.
double year_fraction(Date from, Date to, DayCount day_count);

double interest_amount(double balance, double annual_rate, double fraction);

// Платёж аннуитета при начислении rate/12 в месяц.
double annuity_payment(double principal, double annual_rate, int payments);

// Ставка с учётом ограничений: min(max(rate, floor), cap).
double effective_rate(double rate, std::optional<double> floor, std::optional<double> cap);

// Последнее значение не позже даты. fixings отсортированы по дате.
// Если такого значения нет, результат пустой: подставлять 0 нельзя.
std::optional<double> last_known_rate(std::span<const RateFixing> fixings, Date date);

// Уменьшает остаток и возвращает фактически погашенную сумму в пределах [0, balance].
double apply_principal(double& balance, double amount);

// Округление до копеек, половина от нуля. Только для вывода: внутри считаем без округления.
double round_money(double amount);

// Сумма с компенсацией ошибки округления (алгоритм Ньюмана).
class NeumaierSum {
public:
    void add(double x) {
        const double t = sum_ + x;
        if (std::fabs(sum_) >= std::fabs(x)) {
            compensation_ += (sum_ - t) + x;
        } else {
            compensation_ += (x - t) + sum_;
        }
        sum_ = t;
    }

    void merge(const NeumaierSum& other) {
        add(other.sum_);
        add(other.compensation_);
    }

    double value() const { return sum_ + compensation_; }

private:
    double sum_ = 0.0;
    double compensation_ = 0.0;
};

}  // namespace cfcore
