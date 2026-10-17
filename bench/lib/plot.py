"""Matplotlib setup and helpers shared by the chart scripts (SVG output)."""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter

plt.rcParams.update({
    "font.family": ["DejaVu Sans"],
    "axes.unicode_minus": False, "svg.fonttype": "none", "font.size": 10,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.alpha": 0.3, "axes.axisbelow": True,
})
LEG = dict(frameon=True, facecolor="white", edgecolor="none", framealpha=1)


def saver(out):
    """Return save(fig, name), which writes OUT/name.svg and closes fig."""
    os.makedirs(out, exist_ok=True)

    def save(fig, name):
        fig.savefig(os.path.join(out, name + ".svg"), bbox_inches="tight")
        plt.close(fig)
        print("wrote", name)
    return save


def title(ax, t, sub):
    ax.set_title(t, fontsize=13, fontweight="bold", loc="left", pad=24 + 12 * sub.count("\n"))
    ax.annotate(sub, xy=(0, 1), xycoords="axes fraction", xytext=(0, 8), textcoords="offset points",
                fontsize=9, color="#475569", va="bottom", ha="left")


def plain_log(axis):
    """Log axis with plain-number tick labels (1, 2, 5, 10, ...)."""
    axis.set_major_locator(LogLocator(base=10, subs=(1, 2, 5)))
    axis.set_major_formatter(FuncFormatter(lambda v, _: "%g" % v))
    axis.set_minor_formatter(NullFormatter())
