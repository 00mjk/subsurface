// SPDX-License-Identifier: GPL-2.0
#include "statsview.h"
#include "barseries.h"
#include "boxseries.h"
#include "scatterseries.h"
#include "statsaxis.h"
#include "statsstate.h"
#include "statstranslations.h"
#include "statstypes.h"
#include "core/divefilter.h"
#include "core/subsurface-qt/divelistnotifier.h"
#include <QQuickItem>
#include <QBarSet>
#include <QBarSeries>
#include <QChart>
#include <QGraphicsSceneHoverEvent>
#include <QHorizontalBarSeries>
#include <QHorizontalStackedBarSeries>
#include <QLineSeries>
#include <QLocale>
#include <QPieSeries>
#include <QStackedBarSeries>

// Constants that control the graph layouts
static const QColor quartileMarkerColor(Qt::red);
static const double quartileMarkerSize = 15;

static const QUrl urlStatsView = QUrl(QStringLiteral("qrc:/qml/statsview.qml"));

// We use QtQuick's ChartView so that we can show the statistics on mobile.
// However, accessing the ChartView from C++ is maliciously cumbersome and
// the full QChart interface is not exported. Fortunately, the interface
// leaks the QChart object: We can create a dummy-series and access the chart
// object via the chart() accessor function. By creating a "PieSeries", the
// ChartView does not automatically add axes.
static QtCharts::QChart *getChart(QQuickItem *item)
{
	QtCharts::QAbstractSeries *abstract_series;
	if (!item)
		return nullptr;
	if (!QMetaObject::invokeMethod(item, "createSeries", Qt::AutoConnection,
				       Q_RETURN_ARG(QtCharts::QAbstractSeries *, abstract_series),
				       Q_ARG(int, QtCharts::QAbstractSeries::SeriesTypePie),
				       Q_ARG(QString, QString()))) {
		qWarning("Couldn't call createSeries()");
		return nullptr;
	}
	QtCharts::QChart *res = abstract_series->chart();
	res->removeSeries(abstract_series);
	delete abstract_series;
	return res;
}

bool StatsView::EventFilter::eventFilter(QObject *o, QEvent *event)
{
	if (event->type() == QEvent::GraphicsSceneHoverMove) {
		QGraphicsSceneHoverEvent *hover = static_cast<QGraphicsSceneHoverEvent *>(event);
		view->hover(hover->pos());
		return true;
	}
	return QObject::eventFilter(o, event);
}

StatsView::StatsView(QWidget *parent) : QQuickWidget(parent),
	highlightedScatterSeries(nullptr),
	highlightedBarSeries(nullptr),
	highlightedBoxSeries(nullptr),
	eventFilter(this)
{
	setResizeMode(QQuickWidget::SizeRootObjectToView);
	setSource(urlStatsView);
	chart = getChart(rootObject());
	connect(chart, &QtCharts::QChart::plotAreaChanged, this, &StatsView::plotAreaChanged);
	connect(&diveListNotifier, &DiveListNotifier::numShownChanged, this, &StatsView::replotIfVisible);

	chart->installEventFilter(&eventFilter);
	chart->setAcceptHoverEvents(true);
}

StatsView::~StatsView()
{
}

void StatsView::plotAreaChanged(const QRectF &)
{
	for (auto &axis: axes)
		axis->updateLabels(chart);
	for (auto &series: scatterSeries)
		series->updatePositions();
	for (auto &series: barSeries)
		series->updatePositions();
	for (auto &series: boxSeries)
		series->updatePositions();
	for (QuartileMarker &marker: quartileMarkers)
		marker.updatePosition();
}

void StatsView::replotIfVisible()
{
	if (isVisible())
		plot(state);
}

// Generic code to handle the highlighting of a series element
template<typename Series>
void StatsView::handleHover(const std::vector<std::unique_ptr<Series>> &series, Series *&highlighted, QPointF pos)
{
	// For bar series, we simply take the first bar under the mouse, as
	// bars shouldn't overlap.
	int nextItem = -1;
	Series *nextSeries = nullptr;
	for (auto &series: series) {
		if ((nextItem = series->getItemUnderMouse(pos)) >= 0) {
			nextSeries = series.get();
			break;
		}
	}

	// If there was a different series with a highlighted item - unhighlight it
	if (highlighted && nextSeries != highlighted)
		highlighted->highlight(-1, pos);

	highlighted = nextSeries;
	if (highlighted)
		highlighted->highlight(nextItem, pos);
}

void StatsView::hover(QPointF pos)
{
	// Currently, we don't different series in the same plot.
	// Therefore, treat these cases separately.

	// Get closest scatter item
	ScatterSeries *nextSeries = nullptr;
	int nextItem = -1;
	double nextDistance = 1e14;
	for (auto &series: scatterSeries) {
		auto [dist, index] = series->getClosest(pos);
		if (index >= 0 && dist < nextDistance) {
			nextSeries = series.get();
			nextItem = index;
			nextDistance = dist;
		}
	}

	// If there was a different series with a highlighted item - unhighlight it
	if (highlightedScatterSeries && nextSeries != highlightedScatterSeries)
		highlightedScatterSeries->highlight(-1);

	highlightedScatterSeries = nextSeries;
	if (highlightedScatterSeries)
		highlightedScatterSeries->highlight(nextItem);

	handleHover(barSeries, highlightedBarSeries, pos);
	handleHover(boxSeries, highlightedBoxSeries, pos);
}

void StatsView::initSeries(QtCharts::QAbstractSeries *series, const QString &name)
{
	series->setName(name);
	chart->addSeries(series);
	if (axes.size() >= 2) {
		// Not all charts have axes (e.g. Pie charts)
		series->attachAxis(axes[0]->qaxis());
		series->attachAxis(axes[1]->qaxis());
	}
}

template<typename Type>
Type *StatsView::addSeries(const QString &name)
{
	Type *res = new Type;
	initSeries(res, name);
	return res;
}

ScatterSeries *StatsView::addScatterSeries(const QString &name, const StatsType &typeX, const StatsType &typeY)
{
	scatterSeries.emplace_back(new ScatterSeries(typeX, typeY));
	initSeries(scatterSeries.back().get(), name);
	return scatterSeries.back().get();
}

BarSeries *StatsView::addBarSeries(const QString &name, bool horizontal, const QString &categoryName,
				   const StatsType *valueType)
{
	barSeries.emplace_back(new BarSeries(horizontal, categoryName, valueType));
	initSeries(barSeries.back().get(), name);
	return barSeries.back().get();
}

BoxSeries *StatsView::addBoxSeries(const QString &name, const QString &unit, int decimals)
{
	boxSeries.emplace_back(new BoxSeries(name, unit, decimals));
	initSeries(boxSeries.back().get(), name);
	return boxSeries.back().get();
}

void StatsView::showLegend()
{
	QtCharts::QLegend *legend = chart->legend();
	if (!legend)
		return;
	legend->setVisible(true);
	legend->setAlignment(Qt::AlignBottom);
}

void StatsView::hideLegend()
{
	QtCharts::QLegend *legend = chart->legend();
	if (!legend)
		return;
	legend->setVisible(false);
}

void StatsView::setTitle(const QString &s)
{
	chart->setTitle(s);
}

template <typename T, class... Args>
T *StatsView::createAxis(const QString &title, Args&&... args)
{
	T *res = new T(std::forward<Args>(args)...);
	axes.emplace_back(res);
	axes.back()->updateLabels(chart);
	axes.back()->qaxis()->setTitleText(title);
	return res;
}

void StatsView::addAxes(StatsAxis *x, StatsAxis *y)
{
	chart->addAxis(x->qaxis(), Qt::AlignBottom);
	chart->addAxis(y->qaxis(), Qt::AlignLeft);
}

void StatsView::reset()
{
	if (!chart)
		return;
	highlightedScatterSeries = nullptr;
	highlightedBarSeries = nullptr;
	highlightedBoxSeries = nullptr;
	scatterSeries.clear();
	barSeries.clear();
	boxSeries.clear();
	quartileMarkers.clear();
	chart->removeAllSeries();
	axes.clear();
}

void StatsView::plot(const StatsState &stateIn)
{
	state = stateIn;
	if (!chart || !state.var1)
		return;
	reset();

	const std::vector<dive *> dives = DiveFilter::instance()->visibleDives();
	switch (state.type) {
	case ChartType::DiscreteBar:
		return plotBarChart(dives, state.subtype, state.var1, state.var1Binner, state.var2, state.var2Binner);
	case ChartType::DiscreteValue:
		return plotValueChart(dives, state.subtype, state.var1, state.var1Binner, state.var2,
				      state.var2Operation, state.labels);
	case ChartType::DiscreteCount:
		return plotDiscreteCountChart(dives, state.subtype, state.var1, state.var1Binner, state.labels);
	case ChartType::Pie:
		return plotPieChart(dives, state.var1, state.var1Binner);
	case ChartType::DiscreteBox:
		return plotDiscreteBoxChart(dives, state.var1, state.var1Binner, state.var2);
	case ChartType::DiscreteScatter:
		return plotDiscreteScatter(dives, state.var1, state.var1Binner, state.var2, state.quartiles);
	case ChartType::HistogramCount:
		return plotHistogramCountChart(dives, state.subtype, state.var1, state.var1Binner,
					       state.labels, state.median, state.mean);
	case ChartType::HistogramBar:
		return plotHistogramBarChart(dives, state.subtype, state.var1, state.var1Binner, state.var2,
					     state.var2Operation, state.labels);
	case ChartType::HistogramBox:
		return plotHistogramBoxChart(dives, state.var1, state.var1Binner, state.var2);
	case ChartType::ScatterPlot:
		return plotScatter(dives, state.var1, state.var2);
	default:
		qWarning("Unknown chart type: %d", (int)state.type);
		return;
	}
}

template<typename T>
CategoryAxis *StatsView::createCategoryAxis(const QString &name, const StatsBinner &binner,
					    const std::vector<T> &bins, bool isHorizontal)
{
	std::vector<QString> labels;
	labels.reserve(bins.size());
	for (const auto &[bin, dummy]: bins)
		labels.push_back(binner.format(*bin));
	return createAxis<CategoryAxis>(name, labels, isHorizontal);
}

CountAxis *StatsView::createCountAxis(int maxVal, bool isHorizontal)
{
	return createAxis<CountAxis>(StatsTranslations::tr("No. dives"), maxVal, isHorizontal);
}

void StatsView::plotBarChart(const std::vector<dive *> &dives,
			     ChartSubType subType,
			     const StatsType *categoryType, const StatsBinner *categoryBinner,
			     const StatsType *valueType, const StatsBinner *valueBinner)
{
	using QtCharts::QBarSet;
	using QtCharts::QAbstractBarSeries;

	if (!categoryBinner || !valueBinner)
		return;

	setTitle(valueType->nameWithBinnerUnit(*valueBinner));

	std::vector<StatsBinDives> categoryBins = categoryBinner->bin_dives(dives, false);

	// The problem here is that for different dive sets of the category
	// bins, we might get different value bins. So we have to keep track
	// of our counts and adjust accordingly. That's a bit annoying.
	// Perhaps we should determine the bins of all dives first and then
	// query the counts for precisely those bins?
	using BinCountsPair = std::pair<StatsBinPtr, std::vector<int>>;
	std::vector<BinCountsPair> vbin_counts;
	int catBinNr = 0;
	int maxCount = 0;
	int maxCategoryCount = 0;
	for (const auto &[bin, dives]: categoryBins) {
		int categoryCount = 0;
		for (auto &[vbin, count]: valueBinner->count_dives(dives, false)) {
			// Note: we assume that the bins are sorted!
			auto it = std::lower_bound(vbin_counts.begin(), vbin_counts.end(), vbin,
						   [] (const BinCountsPair &p, const StatsBinPtr &bin)
						   { return *p.first < *bin; });
			if (it == vbin_counts.end() || *it->first != *vbin) {
				// Add a new value bin.
				// Attn: this invalidate "vbin", which must not be used henceforth!
				it = vbin_counts.insert(it, { std::move(vbin), std::vector<int>(categoryBins.size()) });
			}
			it->second[catBinNr] = count;
			categoryCount += count;
			if (count > maxCount)
				maxCount = count;
		}
		if (categoryCount > maxCategoryCount)
			maxCategoryCount = categoryCount;
		++catBinNr;
	}

	bool isStacked = subType == ChartSubType::VerticalStacked || subType == ChartSubType::HorizontalStacked;
	bool isHorizontal = subType == ChartSubType::Horizontal || subType == ChartSubType::HorizontalStacked;

	CategoryAxis *catAxis = createCategoryAxis(categoryType->nameWithBinnerUnit(*categoryBinner),
						   *categoryBinner, categoryBins, !isHorizontal);

	int maxVal = isStacked ? maxCategoryCount : maxCount;
	CountAxis *valAxis = createCountAxis(maxVal, isHorizontal);

	if (isHorizontal)
		addAxes(valAxis, catAxis);
	else
		addAxes(catAxis, valAxis);
	QAbstractBarSeries *series;
	switch (subType) {
	default:
	case ChartSubType::Vertical:
		series = addSeries<QtCharts::QBarSeries>(valueType->name());
		break;
	case ChartSubType::VerticalStacked:
		series = addSeries<QtCharts::QStackedBarSeries>(valueType->name());
		break;
	case ChartSubType::Horizontal:
		series = addSeries<QtCharts::QHorizontalBarSeries>(valueType->name());
		break;
	case ChartSubType::HorizontalStacked:
		series = addSeries<QtCharts::QHorizontalStackedBarSeries>(valueType->name());
		break;
	}

	for (auto &[vbin, counts]: vbin_counts) {
		QBarSet *set = new QBarSet(valueBinner->format(*vbin));
		for (int count: counts)
			*set << count;
		series->append(set);
	}

	showLegend();
}

const double NaN = std::numeric_limits<double>::quiet_NaN();

// These templates are used to extract min and max y-values of various lists.
// A bit too convoluted for my tastes - can we make that simpler?
static std::pair<double, double> getMinMaxValueBase(const std::vector<StatsValue> &values)
{
	// Attention: this supposes that the list is sorted!
	return values.empty() ? std::make_pair(NaN, NaN) : std::make_pair(values.front().v, values.back().v);
}
static std::pair<double, double> getMinMaxValueBase(double v)
{
	return { v, v };
}
static std::pair<double, double> getMinMaxValueBase(const StatsQuartiles &q)
{
	return { q.min, q.max };
}
static std::pair<double, double> getMinMaxValueBase(const StatsScatterItem &s)
{
	return { s.y, s.y };
}
template <typename T1, typename T2>
static std::pair<double, double> getMinMaxValueBase(const std::pair<T1, T2> &p)
{
	return getMinMaxValueBase(p.second);
}
template <typename T>
static std::pair<double, double> getMinMaxValueBase(const StatsBinValue<T> &v)
{
	return getMinMaxValueBase(v.value);
}

template <typename T>
static void updateMinMax(double &min, double &max, bool &found, const T &v)
{
	const auto [mi, ma] = getMinMaxValueBase(v);
	if (!std::isnan(mi) && mi < min)
		min = mi;
	if (!std::isnan(ma) && ma > max)
		max = ma;
	if (!std::isnan(mi) || !std::isnan(ma))
		found = true;
}

template <typename T>
static std::pair<double, double> getMinMaxValue(const std::vector<T> &values)
{
	double min = 1e14, max = 0.0;
	bool found = false;
	for (const T &v: values)
		updateMinMax(min, max, found, v);
	return found ? std::make_pair(min, max) : std::make_pair(0.0, 0.0);
}

static std::pair<double, double> getMinMaxValue(const std::vector<StatsBinOp> &bins, StatsOperation op)
{
	double min = 1e14, max = 0.0;
	bool found = false;
	for (auto &[bin, res]: bins) {
		if (!res.isValid())
			continue;
		updateMinMax(min, max, found, res.get(op));
	}
	return found ? std::make_pair(min, max) : std::make_pair(0.0, 0.0);
}

void StatsView::plotValueChart(const std::vector<dive *> &dives,
			       ChartSubType subType,
			       const StatsType *categoryType, const StatsBinner *categoryBinner,
			       const StatsType *valueType, StatsOperation valueAxisOperation,
			       bool labels)
{
	if (!categoryBinner)
		return;

	setTitle(QStringLiteral("%1 (%2)").arg(valueType->name(), StatsType::operationName(valueAxisOperation)));

	std::vector<StatsBinOp> categoryBins = valueType->bin_operations(*categoryBinner, dives, false);

	// If there is nothing to display, quit
	if (categoryBins.empty())
		return;


	bool isHorizontal = subType == ChartSubType::Horizontal;
	const auto [minValue, maxValue] = getMinMaxValue(categoryBins, valueAxisOperation);
	int decimals = valueType->decimals();
	CategoryAxis *catAxis = createCategoryAxis(categoryType->nameWithBinnerUnit(*categoryBinner),
						   *categoryBinner, categoryBins, !isHorizontal);
	ValueAxis *valAxis = createAxis<ValueAxis>(valueType->nameWithUnit(),
						   0.0, maxValue, valueType->decimals(), isHorizontal);

	if (isHorizontal)
		addAxes(valAxis, catAxis);
	else
		addAxes(catAxis, valAxis);

	double pos = 0.0;
	BarSeries *series = addBarSeries(QString(), isHorizontal, categoryType->name(), valueType);
	QString unit = valueType->unitSymbol();
	for (auto &[bin, res]: categoryBins) {
		if (res.isValid()) {
			double height = res.get(valueAxisOperation);
			QString value = QString("%L1").arg(height, 0, 'f', decimals);
			std::vector<QString> label = labels ? std::vector<QString> { value }
							    : std::vector<QString>();
			series->append(pos - 0.5, pos + 0.5, height, label,
				       categoryBinner->formatWithUnit(*bin), res);
		}
		pos += 1.0;
	}

	hideLegend();
}

static int getTotalCount(const std::vector<StatsBinCount> &bins)
{
	int total = 0;
	for (const auto &[bin, count]: bins)
		total += count;
	return total;
}

// Formats "x (y%)" as either a single or two strings for horizontal and non-horizontal cases, respectively.
static std::vector<QString> makePercentageLabels(int count, int total, bool isHorizontal)
{
	double percentage = count * 100.0 / total;
	QString countString = QString("%L1").arg(count);
	QString percentageString = QString("%L1%").arg(percentage, 0, 'f', 1);
	if (isHorizontal)
		return { QString("%1 %2").arg(countString, percentageString) };
	else
		return { countString, percentageString };
}

static QString makePiePercentageLabel(const QString &bin, int count, int total)
{
	QLocale loc;
	double percentage = count * 100.0 / total;
	return QString("%1 (%2: %3\%)").arg(
			bin,
			loc.toString(count),
			loc.toString(percentage, 'f', 1));
}

template<typename T>
static int getMaxCount(const std::vector<T> &bins)
{
	int res = 0;
	for (auto const &[dummy, val]: bins) {
		if (val > res)
			res = val;
	}
	return res;
}

void StatsView::plotDiscreteCountChart(const std::vector<dive *> &dives,
				      ChartSubType subType,
				      const StatsType *categoryType, const StatsBinner *categoryBinner,
				      bool labels)
{
	if (!categoryBinner)
		return;

	setTitle(categoryType->nameWithBinnerUnit(*categoryBinner));

	std::vector<StatsBinCount> categoryBins = categoryBinner->count_dives(dives, false);

	// If there is nothing to display, quit
	if (categoryBins.empty())
		return;

	int total = getTotalCount(categoryBins);
	bool isHorizontal = subType != ChartSubType::Vertical;

	CategoryAxis *catAxis = createCategoryAxis(categoryType->nameWithBinnerUnit(*categoryBinner),
						   *categoryBinner, categoryBins, !isHorizontal);

	int maxCount = getMaxCount(categoryBins);
	CountAxis *valAxis = createCountAxis(maxCount, isHorizontal);

	if (isHorizontal)
		addAxes(valAxis, catAxis);
	else
		addAxes(catAxis, valAxis);

	double pos = 0.0;
	BarSeries *series = addBarSeries(QString(), isHorizontal, categoryType->name(), nullptr);
	for (auto const &[bin, count]: categoryBins) {
		std::vector<QString> label = labels ? makePercentageLabels(count, total, isHorizontal)
						    : std::vector<QString>();
		series->append(pos - 0.5, pos + 0.5, count, label,
			       categoryBinner->formatWithUnit(*bin), total);
		pos += 1.0;
	}

	hideLegend();
}

void StatsView::plotPieChart(const std::vector<dive *> &dives,
			     const StatsType *categoryType, const StatsBinner *categoryBinner)
{
	using QtCharts::QPieSeries;
	using QtCharts::QPieSlice;

	if (!categoryBinner)
		return;

	setTitle(categoryType->nameWithBinnerUnit(*categoryBinner));

	std::vector<StatsBinCount> categoryBins = categoryBinner->count_dives(dives, false);

	// If there is nothing to display, quit
	if (categoryBins.empty())
		return;

	int total = getTotalCount(categoryBins);
	QPieSeries *series = addSeries<QtCharts::QPieSeries>(categoryType->name());

	// The Pie chart becomes very slow for a big number of slices.
	// Moreover, it is unreadable. Therefore, subsume slices under a
	// certain percentage as "other". But draw a minimum number of slices
	// until we reach 50% so that we never get a pie only of "other".
	// This is heuristics, which might have to be optimized.
	const int smallest_slice_percentage = 2; // Smaller than 2% = others. That makes at most 50 slices.
	const int min_slices = 10; // Try to draw at least 10 slices until we reach 50%
	std::sort(categoryBins.begin(), categoryBins.end(),
		  [](const StatsBinCount &item1, const StatsBinCount &item2)
		  { return item1.value > item2.value; }); // Note: reverse sort.
	auto it = std::find_if(categoryBins.begin(), categoryBins.end(),
			       [total, smallest_slice_percentage](const StatsBinCount &item)
			       { return item.value * 100 / total < smallest_slice_percentage; });
	if (it - categoryBins.begin() < min_slices) {
		// Take minimum amount of slices below 50%...
		int sum = 0;
		for (auto it2 = categoryBins.begin(); it2 != it; ++it2)
			sum += it2->value;

		while(it != categoryBins.end() &&
		      sum * 2 < total &&
		      it - categoryBins.begin() < min_slices) {
			sum += it->value;
			++it;
		}
	}

	// Sum counts of "other" bins.
	int otherCount = 0;
	for (auto it2 = it; it2 != categoryBins.end(); ++it2)
		otherCount += it2->value;

	categoryBins.erase(it, categoryBins.end()); // Delete "other" bins

	for (auto const &[bin, count]: categoryBins) {
		QString label = makePiePercentageLabel(categoryBinner->format(*bin), count, total);
		QPieSlice *slice = new QPieSlice(label, count);
		slice->setLabelVisible(true);
		series->append(slice);
	}
	if (otherCount) {
		QString label = makePiePercentageLabel(StatsTranslations::tr("other"), otherCount, total);
		QPieSlice *slice = new QPieSlice(label, otherCount);
		slice->setLabelVisible(true);
		series->append(slice);
	}
	showLegend();
}

void StatsView::plotDiscreteBoxChart(const std::vector<dive *> &dives,
				     const StatsType *categoryType, const StatsBinner *categoryBinner,
				     const StatsType *valueType)
{
	if (!categoryBinner)
		return;

	setTitle(valueType->name());

	std::vector<StatsBinQuartiles> categoryBins = valueType->bin_quartiles(*categoryBinner, dives, false);

	// If there is nothing to display, quit
	if (categoryBins.empty())
		return;

	CategoryAxis *catAxis = createCategoryAxis(categoryType->nameWithBinnerUnit(*categoryBinner),
						   *categoryBinner, categoryBins, true);

	auto [minY, maxY] = getMinMaxValue(categoryBins);
	ValueAxis *valueAxis = createAxis<ValueAxis>(valueType->nameWithUnit(),
						     minY, maxY, valueType->decimals(), false);

	addAxes(catAxis, valueAxis);

	BoxSeries *series = addBoxSeries(valueType->name(), valueType->unitSymbol(), valueType->decimals());

	double pos = 0.0;
	for (auto &[bin, q]: categoryBins) {
		if (q.isValid())
			series->append(pos - 0.5, pos + 0.5, q, categoryBinner->formatWithUnit(*bin));
		pos += 1.0;
	}

	hideLegend();
}

void StatsView::plotDiscreteScatter(const std::vector<dive *> &dives,
				    const StatsType *categoryType, const StatsBinner *categoryBinner,
				    const StatsType *valueType, bool quartiles)
{
	if (!categoryBinner)
		return;

	setTitle(valueType->name());

	std::vector<StatsBinValues> categoryBins = valueType->bin_values(*categoryBinner, dives, false);

	// If there is nothing to display, quit
	if (categoryBins.empty())
		return;

	CategoryAxis *catAxis = createCategoryAxis(categoryType->nameWithBinnerUnit(*categoryBinner),
			 			   *categoryBinner, categoryBins, true);

	auto [minValue, maxValue] = getMinMaxValue(categoryBins);

	ValueAxis *valAxis = createAxis<ValueAxis>(valueType->nameWithUnit(),
						   minValue, maxValue, valueType->decimals(), false);

	addAxes(catAxis, valAxis);
	ScatterSeries *series = addScatterSeries(valueType->name(), *categoryType, *valueType);

	double x = 0.0;
	for (const auto &[bin, array]: categoryBins) {
		for (auto [v, d]: array)
			series->append(d, x, v);
		if (quartiles) {
			StatsQuartiles quartiles = StatsType::quartiles(array);
			if (quartiles.isValid()) {
				quartileMarkers.emplace_back(x, quartiles.q1, series);
				quartileMarkers.emplace_back(x, quartiles.q2, series);
				quartileMarkers.emplace_back(x, quartiles.q3, series);
			}
		}
		x += 1.0;
	}

	hideLegend();
}

void StatsView::addLineMarker(double pos, double low, double high, const QPen &pen, bool isHorizontal)
{
	using QtCharts::QLineSeries;

	QLineSeries *series = addSeries<QLineSeries>(QString());
	if(!series)
		return;
	if (isHorizontal) {
		series->append(low, pos);
		series->append(high, pos);
	} else {
		series->append(pos, low);
		series->append(pos, high);
	}
	series->setPen(pen);
}

StatsView::QuartileMarker::QuartileMarker(double pos, double value, QtCharts::QAbstractSeries *series) :
	item(new QGraphicsLineItem(series->chart())),
	series(series),
	pos(pos),
	value(value)
{
	item->setZValue(10.0); // ? What is a sensible value here ?
	item->setPen(QPen(quartileMarkerColor, 2.0));
	updatePosition();
}

void StatsView::QuartileMarker::updatePosition()
{
	QtCharts::QChart *chart = series->chart();
	QPointF center = chart->mapToPosition(QPointF(pos, value), series);
	item->setLine(center.x() - quartileMarkerSize / 2.0, center.y(),
		      center.x() + quartileMarkerSize / 2.0, center.y());
}

// Yikes, we get our data in different kinds of (bin, value) pairs.
// To create a category axis from this, we have to templatify the function.
template<typename T>
HistogramAxis *StatsView::createHistogramAxis(const QString &name, const StatsBinner &binner,
					      const std::vector<T> &bins, bool isHorizontal)
{
	std::vector<HistogramAxisEntry> labels;
	for (auto const &[bin, dummy]: bins) {
		QString label = binner.formatLowerBound(*bin);
		double lowerBound = binner.lowerBoundToFloat(*bin);
		bool prefer = binner.preferBin(*bin);
		labels.push_back({ label, lowerBound, prefer });
	}

	const StatsBin &lastBin = *bins.back().bin;
	QString lastLabel = binner.formatUpperBound(lastBin);
	double upperBound = binner.upperBoundToFloat(lastBin);
	labels.push_back({ lastLabel, upperBound, false });

	return createAxis<HistogramAxis>(name, std::move(labels), isHorizontal);
}

void StatsView::plotHistogramCountChart(const std::vector<dive *> &dives,
					ChartSubType subType,
					const StatsType *categoryType, const StatsBinner *categoryBinner,
					bool labels, bool showMedian, bool showMean)
{
	if (!categoryBinner)
		return;

	setTitle(categoryType->name());

	std::vector<StatsBinCount> categoryBins = categoryBinner->count_dives(dives, true);

	// If there is nothing to display, quit
	if (categoryBins.empty())
		return;

	bool isHorizontal = subType == ChartSubType::Horizontal;
	HistogramAxis *catAxis = createHistogramAxis(categoryType->nameWithBinnerUnit(*categoryBinner),
						     *categoryBinner, categoryBins, !isHorizontal);

	int maxCategoryCount = getMaxCount(categoryBins);
	int total = getTotalCount(categoryBins);

	StatsAxis *valAxis = createCountAxis(maxCategoryCount, isHorizontal);
	double chartHeight = valAxis->minMax().second;

	if (isHorizontal)
		addAxes(valAxis, catAxis);
	else
		addAxes(catAxis, valAxis);

	BarSeries *series = addBarSeries(QString(), isHorizontal, categoryType->name(), nullptr);
	for (auto const &[bin, count]: categoryBins) {
		double lowerBound = categoryBinner->lowerBoundToFloat(*bin);
		double upperBound = categoryBinner->upperBoundToFloat(*bin);
		std::vector<QString> label = labels ? makePercentageLabels(count, total, isHorizontal)
						    : std::vector<QString>();

		series->append(lowerBound, upperBound, count, label,
			       categoryBinner->formatWithUnit(*bin), total);
	}

	if (categoryType->type() == StatsType::Type::Numeric) {
		if (showMean) {
			double mean = categoryType->mean(dives);
			QPen meanPen(Qt::green);
			meanPen.setWidth(2);
			if (!std::isnan(mean))
				addLineMarker(mean, 0.0, chartHeight, meanPen, isHorizontal);
		}
		if (showMedian) {
			double median = categoryType->quartiles(dives).q2;
			QPen medianPen(Qt::red);
			medianPen.setWidth(2);
			if (!std::isnan(median))
				addLineMarker(median, 0.0, chartHeight, medianPen, isHorizontal);
		}
	}

	hideLegend();
}

void StatsView::plotHistogramBarChart(const std::vector<dive *> &dives,
				      ChartSubType subType,
				      const StatsType *categoryType, const StatsBinner *categoryBinner,
				      const StatsType *valueType, StatsOperation valueAxisOperation,
				      bool labels)
{
	if (!categoryBinner)
		return;

	setTitle(QStringLiteral("%1 (%2)").arg(valueType->name(), StatsType::operationName(valueAxisOperation)));

	std::vector<StatsBinOp> categoryBins = valueType->bin_operations(*categoryBinner, dives, true);

	// If there is nothing to display, quit
	if (categoryBins.empty())
		return;

	bool isHorizontal = subType == ChartSubType::Horizontal;
	HistogramAxis *catAxis = createHistogramAxis(categoryType->nameWithBinnerUnit(*categoryBinner),
						     *categoryBinner, categoryBins, !isHorizontal);

	const auto [minValue, maxValue] = getMinMaxValue(categoryBins, valueAxisOperation);

	int decimals = valueType->decimals();
	ValueAxis *valAxis = createAxis<ValueAxis>(valueType->nameWithUnit(),
						   0.0, maxValue, decimals, isHorizontal);

	if (isHorizontal)
		addAxes(valAxis, catAxis);
	else
		addAxes(catAxis, valAxis);

	BarSeries *series = addBarSeries(QString(), isHorizontal, categoryType->name(), valueType);
	QString unit = valueType->unitSymbol();
	for (auto const &[bin, res]: categoryBins) {
		if (!res.isValid())
			continue;
		double height = res.get(valueAxisOperation);
		double lowerBound = categoryBinner->lowerBoundToFloat(*bin);
		double upperBound = categoryBinner->upperBoundToFloat(*bin);
		QString value = QString("%L1").arg(height, 0, 'f', decimals);
		std::vector<QString> label = labels ? std::vector<QString> { value }
						    : std::vector<QString>();
		series->append(lowerBound, upperBound, height, label,
			       categoryBinner->formatWithUnit(*bin), res);
	}

	hideLegend();
}

void StatsView::plotHistogramBoxChart(const std::vector<dive *> &dives,
				      const StatsType *categoryType, const StatsBinner *categoryBinner,
				      const StatsType *valueType)
{
	if (!categoryBinner)
		return;

	setTitle(valueType->name());

	std::vector<StatsBinQuartiles> categoryBins = valueType->bin_quartiles(*categoryBinner, dives, true);

	// If there is nothing to display, quit
	if (categoryBins.empty())
		return;

	HistogramAxis *catAxis = createHistogramAxis(categoryType->nameWithBinnerUnit(*categoryBinner),
						     *categoryBinner, categoryBins, true);

	auto [minY, maxY] = getMinMaxValue(categoryBins);
	ValueAxis *valueAxis = createAxis<ValueAxis>(valueType->nameWithUnit(),
						     minY, maxY, valueType->decimals(), false);

	addAxes(catAxis, valueAxis);

	BoxSeries *series = addBoxSeries(valueType->name(), valueType->unitSymbol(), valueType->decimals());

	for (auto &[bin, q]: categoryBins) {
		if (!q.isValid())
			continue;
		double lowerBound = categoryBinner->lowerBoundToFloat(*bin);
		double upperBound = categoryBinner->upperBoundToFloat(*bin);
		series->append(lowerBound, upperBound, q, categoryBinner->formatWithUnit(*bin));
	}

	hideLegend();
}

static bool is_linear_regression(int sample_size, double cov, double sx2, double sy2)
{
	// One point never, two points always form a line
	if (sample_size < 2)
		return false;
	if (sample_size <= 2)
		return true;

	const double tval[] = { 12.709, 4.303, 3.182, 2.776, 2.571, 2.447, 2.201, 2.120, 2.080,  2.056, 2.021, 1.960,  1.960 };
	const int t_df[] =    { 1,      2,     3,     4,     5,     6,     11,    16,    21,     26,    40,    100,   100000 };
	int df = sample_size - 2;   // Following is the one-tailed t-value at p < 0.05 and [sample_size - 2] degrees of freedom for the dive data:
	double t = (cov / sx2) / sqrt(((sy2 - cov * cov / sx2) / (double)df) / sx2);
	for (int i = std::size(tval) - 2; i >= 0; i--) {   // We do linear interpolation rather than having a large lookup table.
		if (df >= t_df[i]) {    // Look up the appropriate reference t-value at p < 0.05 and df degrees of freedom
			double t_lookup = tval[i] - (tval[i] - tval[i+1]) * (df - t_df[i]) / (t_df[i+1] - t_df[i]);
			return abs(t) >= t_lookup;
		}
	}

	return true; // can't happen, as we tested for sample_size above.
}

// Returns the coefficients [a,b] of the line y = ax + b
// If case of an undetermined regression or one with infinite slope, returns [nan, nan]
static std::pair<double, double> linear_regression(const std::vector<StatsScatterItem> &v)
{
	if (v.size() < 2)
		return { NaN, NaN };

	// First, calculate the x and y average
	double avg_x = 0.0, avg_y = 0.0;
	for (auto [x, y, d]: v) {
		avg_x += x;
		avg_y += y;
	}
	avg_x /= (double)v.size();
	avg_y /= (double)v.size();

	double cov = 0.0, sx2 = 0.0, sy2 = 0.0;
	for (auto [x, y, d]: v) {
		cov += (x - avg_x) * (y - avg_y);
		sx2 += (x - avg_x) * (x - avg_x);
		sy2 += (y - avg_y) * (y - avg_y);
	}

	bool is_linear = is_linear_regression((int)v.size(), cov, sx2, sy2);

	if (fabs(sx2) < 1e-10 || !is_linear) // If t is not statistically significant, do not plot the regression line.
		return { NaN, NaN };
	double a = cov / sx2;
	double b = avg_y - a * avg_x;
	return { a, b };
}

void StatsView::plotScatter(const std::vector<dive *> &dives, const StatsType *categoryType, const StatsType *valueType)
{
	using QtCharts::QLineSeries;

	setTitle(StatsTranslations::tr("%1 vs. %2").arg(valueType->name(), categoryType->name()));

	std::vector<StatsScatterItem> points = categoryType->scatter(*valueType, dives);
	if (points.empty())
		return;

	double minX = points.front().x;
	double maxX = points.back().x;
	auto [minY, maxY] = getMinMaxValue(points);

	StatsAxis *axisX = categoryType->type() == StatsType::Type::Continuous ?
		static_cast<StatsAxis *>(createAxis<DateAxis>(categoryType->nameWithUnit(),
							      minX, maxX, true)) :
		static_cast<StatsAxis *>(createAxis<ValueAxis>(categoryType->nameWithUnit(),
							       minX, maxX, categoryType->decimals(), true));

	StatsAxis *axisY = createAxis<ValueAxis>(valueType->nameWithUnit(), minY, maxY, valueType->decimals(), false);

	addAxes(axisX, axisY);
	ScatterSeries *series = addScatterSeries(valueType->name(), *categoryType, *valueType);

	for (auto [x, y, dive]: points)
		series->append(dive, x, y);

	// y = ax + b
	auto [a, b] = linear_regression(points);
	if (!std::isnan(a)) {
		auto [minx, maxx] = axisX->minMax();
		QLineSeries *series = addSeries<QLineSeries>(QString());
		series->setPen(QPen(Qt::red));
		series->append(minx, a * minx + b);
		series->append(maxx, a * maxx + b);
	}

	hideLegend();
}