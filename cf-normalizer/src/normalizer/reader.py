from pathlib import Path

import polars as pl


def read_csv(path: str | Path) -> pl.DataFrame:
    """Загрузить CSV без преобразования исходных типов данных.

    Args:
        path: Путь к CSV-файлу с разделителем ``;`` в кодировке UTF-8.

    Returns:
        Таблица со строковыми колонками; отсутствующие значения — ``null``.

    Raises:
        OSError: Файл отсутствует или недоступен.
        pl.exceptions.PolarsError: Ошибка разбора CSV.
    """
    return pl.read_csv(
        path,
        separator=";",
        infer_schema=False,
        encoding="utf8",
        glob=False,
    )
