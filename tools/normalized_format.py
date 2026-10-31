"""Колонки нормализованного CSV: разделитель ';', даты ISO, ставки как доли, пусто = нет значения."""

COLUMNS = [
    "deal_id", "report_dttm", "discriminator", "currency_id", "side", "portfolio_key",
    "current_principal_amt", "deal_dttm", "maturity_dttm", "rate_type_cd", "interest_rate",
    "base_indicator_rate", "cap_limit_interest_rate", "floor_limit_interest_rate",
    "accrued_interest", "day_convention_cd",
    "interest_payment_rule_start_dttm", "interest_payment_rule_period_cd", "interest_payment_rule_day_num",
    "regular_payment_rule_start_dttm", "regular_payment_rule_period_cd", "regular_payment_rule_day_num",
    "repricing_rule_start_dttm", "repricing_rule_period_cd", "repricing_rule_day_num",
    "interest_payment", "principal_payment", "repricing_dttm_schedule",
    "repricing_dttm_rate_schedule_fixed",
]
