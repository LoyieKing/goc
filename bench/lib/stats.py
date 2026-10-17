"""Statistics shared by the summarize scripts."""
import math, statistics


def med(xs):
    return statistics.median(xs) if xs else None


def gmean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))
