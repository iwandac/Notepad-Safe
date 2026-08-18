#pragma once

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

// Deterministic, dependency-free pairwise generator for GUI semantic contracts.
// Module tests provide named dimensions and an optional validity constraint;
// the runner returns a compact set that covers every valid pair of values.
// Case ids are stable and can be copied directly from a failure log to replay.
namespace GuiContractMatrix {

struct Dimension {
    QString name;
    QStringList values;
};

using Assignment = QMap<QString, QString>;
using Constraint = std::function<bool(const Assignment &)>;

inline QString caseId(const Assignment &assignment)
{
    QStringList fields;
    for (auto it = assignment.cbegin(); it != assignment.cend(); ++it)
        fields.append(it.key() + QLatin1Char('=') + it.value());
    return fields.join(QLatin1Char(','));
}

inline QVector<Assignment> allValid(const QVector<Dimension> &dimensions,
                                    const Constraint &constraint = {})
{
    QVector<Assignment> rows{Assignment{}};
    for (const Dimension &dimension : dimensions) {
        QVector<Assignment> expanded;
        for (const Assignment &row : std::as_const(rows)) {
            for (const QString &value : dimension.values) {
                Assignment next = row;
                next.insert(dimension.name, value);
                expanded.append(std::move(next));
            }
        }
        rows = std::move(expanded);
    }
    if (!constraint)
        return rows;
    QVector<Assignment> valid;
    for (const Assignment &row : std::as_const(rows)) {
        if (constraint(row))
            valid.append(row);
    }
    return valid;
}

inline QString pairKey(int leftIndex, const QString &left,
                       int rightIndex, const QString &right)
{
    return QString::number(leftIndex) + QLatin1Char(':') + left
        + QLatin1Char('|') + QString::number(rightIndex) + QLatin1Char(':') + right;
}

inline QSet<QString> pairsFor(const Assignment &row,
                              const QVector<Dimension> &dimensions)
{
    QSet<QString> pairs;
    for (int left = 0; left < dimensions.size(); ++left) {
        for (int right = left + 1; right < dimensions.size(); ++right) {
            pairs.insert(pairKey(left, row.value(dimensions.at(left).name),
                                 right, row.value(dimensions.at(right).name)));
        }
    }
    return pairs;
}

inline QVector<Assignment> pairwise(const QVector<Dimension> &dimensions,
                                    const Constraint &constraint = {})
{
    QVector<Assignment> candidates = allValid(dimensions, constraint);
    QSet<QString> uncovered;
    for (const Assignment &row : std::as_const(candidates))
        uncovered.unite(pairsFor(row, dimensions));

    QVector<Assignment> selected;
    while (!uncovered.isEmpty() && !candidates.isEmpty()) {
        int bestIndex = -1;
        int bestScore = -1;
        QString bestId;
        for (int index = 0; index < candidates.size(); ++index) {
            const QSet<QString> covered = pairsFor(candidates.at(index), dimensions);
            int score = 0;
            for (const QString &pair : covered) {
                if (uncovered.contains(pair))
                    ++score;
            }
            const QString id = caseId(candidates.at(index));
            if (score > bestScore || (score == bestScore
                                      && (bestIndex < 0 || id < bestId))) {
                bestIndex = index;
                bestScore = score;
                bestId = id;
            }
        }
        if (bestIndex < 0 || bestScore <= 0)
            break;
        Assignment chosen = candidates.takeAt(bestIndex);
        const QSet<QString> covered = pairsFor(chosen, dimensions);
        for (const QString &pair : covered)
            uncovered.remove(pair);
        selected.append(std::move(chosen));
    }
    return selected;
}

inline bool coversEveryValidPair(const QVector<Assignment> &rows,
                                 const QVector<Dimension> &dimensions,
                                 const Constraint &constraint = {})
{
    QSet<QString> required;
    for (const Assignment &row : allValid(dimensions, constraint))
        required.unite(pairsFor(row, dimensions));
    for (const Assignment &row : rows) {
        const QSet<QString> covered = pairsFor(row, dimensions);
        for (const QString &pair : covered)
            required.remove(pair);
    }
    return required.isEmpty();
}

} // namespace GuiContractMatrix
