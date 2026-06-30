enum class BrushMode { Erase, Restore, Protect };

class MaskCanvas final : public QWidget {
public:
	explicit MaskCanvas(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumSize(420, 320);
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);
		working_ = QImage(kMaskWidth, kMaskHeight, QImage::Format_RGBA8888);
		working_.fill(Qt::transparent);
	}

	void setState(const std::shared_ptr<SmartMatteState> &state)
	{
		if (auto current = state_.lock(); current && state && current->instanceId == state->instanceId)
			return;
		state_ = state;
		working_ = state ? state->maskSnapshot() : QImage(kMaskWidth, kMaskHeight, QImage::Format_RGBA8888);
		if (working_.isNull()) {
			working_ = QImage(kMaskWidth, kMaskHeight, QImage::Format_RGBA8888);
			working_.fill(Qt::transparent);
		}
		undo_.clear();
		redo_.clear();
		zoom_ = 1.0;
		pan_ = {};
		update();
	}

	void setBrushMode(BrushMode mode) { mode_ = mode; }
	void setBrushSize(int size) { brushSize_ = std::clamp(size, 1, 240); update(); }
	void setHardness(int value) { hardness_ = std::clamp(value / 100.0, 0.0, 1.0); }
	void setOpacity(int value) { opacity_ = std::clamp(value / 100.0, 0.01, 1.0); }

	void undo()
	{
		if (undo_.empty())
			return;
		redo_.push_back(working_.copy());
		working_ = undo_.back();
		undo_.pop_back();
		commit(false);
	}

	void redo()
	{
		if (redo_.empty())
			return;
		pushUndo();
		working_ = redo_.back();
		redo_.pop_back();
		commit(false);
	}

	void clearMask()
	{
		pushUndo();
		working_.fill(Qt::transparent);
		commit(true);
	}

	void fitView()
	{
		zoom_ = 1.0;
		pan_ = {};
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), QColor(19, 22, 28));

		const QRectF target = imageRect();
		if (target.width() <= 0 || target.height() <= 0)
			return;

		const int square = 16;
		for (int y = static_cast<int>(target.top()); y < target.bottom(); y += square) {
			for (int x = static_cast<int>(target.left()); x < target.right(); x += square) {
				const bool dark = (((x - static_cast<int>(target.left())) / square) +
						   ((y - static_cast<int>(target.top())) / square)) % 2;
				painter.fillRect(QRect(x, y, square, square), dark ? QColor(45, 48, 56) : QColor(65, 68, 76));
			}
		}

		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
		painter.drawImage(target, displayImage());
		painter.setPen(QPen(QColor(125, 132, 145), 1));
		painter.drawRect(target);

		if (cursorInside_) {
			const double radius = (brushSize_ * target.width() / kMaskWidth) * 0.5;
			painter.setPen(QPen(QColor(255, 255, 255, 220), 1));
			painter.setBrush(Qt::NoBrush);
			painter.drawEllipse(cursorPos_, radius, radius);
		}

		if (state_.expired()) {
			painter.fillRect(target, QColor(0, 0, 0, 135));
			painter.setPen(Qt::white);
			painter.drawText(target, Qt::AlignCenter, obs_module_text("Editor.NoFilter"));
		}
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		cursorPos_ = event->position();
		cursorInside_ = imageRect().contains(cursorPos_);
		if (event->button() == Qt::MiddleButton) {
			panning_ = true;
			lastPanPoint_ = event->position();
			setCursor(Qt::ClosedHandCursor);
			return;
		}
		if (event->button() != Qt::LeftButton || state_.expired() || !cursorInside_)
			return;
		pushUndo();
		redo_.clear();
		painting_ = true;
		lastPaintPoint_ = mapToImage(event->position());
		paintLine(lastPaintPoint_, lastPaintPoint_);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		cursorPos_ = event->position();
		cursorInside_ = imageRect().contains(cursorPos_);
		if (panning_) {
			pan_ += event->position() - lastPanPoint_;
			lastPanPoint_ = event->position();
			update();
			return;
		}
		if (painting_) {
			const QPointF current = mapToImage(event->position());
			paintLine(lastPaintPoint_, current);
			lastPaintPoint_ = current;
		}
		update();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::MiddleButton && panning_) {
			panning_ = false;
			unsetCursor();
		}
		if (event->button() == Qt::LeftButton && painting_) {
			painting_ = false;
			commit(true);
		}
	}

	void leaveEvent(QEvent *) override
	{
		cursorInside_ = false;
		update();
	}

	void enterEvent(QEnterEvent *event) override
	{
		cursorPos_ = event->position();
		cursorInside_ = imageRect().contains(cursorPos_);
		update();
	}

	void wheelEvent(QWheelEvent *event) override
	{
		const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
		zoom_ = std::clamp(zoom_ * factor, 0.5, 8.0);
		update();
		event->accept();
	}

private:
	QRectF imageRect() const
	{
		const QSizeF available(std::max(1, width() - 24), std::max(1, height() - 24));
		const double fit = std::min(available.width() / kMaskWidth, available.height() / kMaskHeight);
		const QSizeF size(kMaskWidth * fit * zoom_, kMaskHeight * fit * zoom_);
		const QPointF center = rect().center() + pan_;
		return {center.x() - size.width() * 0.5, center.y() - size.height() * 0.5, size.width(), size.height()};
	}

	QPointF mapToImage(const QPointF &point) const
	{
		const QRectF target = imageRect();
		const double x = (point.x() - target.left()) / std::max(1.0, target.width()) * kMaskWidth;
		const double y = (point.y() - target.top()) / std::max(1.0, target.height()) * kMaskHeight;
		return {std::clamp(x, 0.0, static_cast<double>(kMaskWidth - 1)),
			std::clamp(y, 0.0, static_cast<double>(kMaskHeight - 1))};
	}

	QImage displayImage() const
	{
		QImage preview = working_.copy();
		for (int y = 0; y < preview.height(); ++y) {
			uchar *row = preview.scanLine(y);
			for (int x = 0; x < preview.width(); ++x) {
				uchar *px = row + x * 4;
				const int r = px[0];
				const int g = px[1];
				px[2] = 0;
				px[3] = static_cast<uchar>(std::min(180, std::max(r, g)));
			}
		}
		return preview;
	}

	void pushUndo()
	{
		undo_.push_back(working_.copy());
		if (static_cast<int>(undo_.size()) > kMaxUndoSteps)
			undo_.erase(undo_.begin());
	}

	void paintLine(const QPointF &from, const QPointF &to)
	{
		const double distance = std::hypot(to.x() - from.x(), to.y() - from.y());
		const double spacing = std::max(1.0, brushSize_ * 0.16);
		const int steps = std::max(1, static_cast<int>(std::ceil(distance / spacing)));
		for (int i = 0; i <= steps; ++i) {
			const double t = static_cast<double>(i) / steps;
			paintDab(from + (to - from) * t);
		}
		commit(false);
	}

	void paintDab(const QPointF &center)
	{
		const double radius = std::max(0.5, brushSize_ * 0.5);
		const int minX = std::max(0, static_cast<int>(std::floor(center.x() - radius)));
		const int maxX = std::min(kMaskWidth - 1, static_cast<int>(std::ceil(center.x() + radius)));
		const int minY = std::max(0, static_cast<int>(std::floor(center.y() - radius)));
		const int maxY = std::min(kMaskHeight - 1, static_cast<int>(std::ceil(center.y() + radius)));
		const double hardEdge = hardness_ * radius;
		const double softWidth = std::max(0.001, radius - hardEdge);

		for (int y = minY; y <= maxY; ++y) {
			auto *row = working_.scanLine(y);
			for (int x = minX; x <= maxX; ++x) {
				const double d = std::hypot((x + 0.5) - center.x(), (y + 0.5) - center.y());
				if (d > radius)
					continue;
				double falloff = 1.0;
				if (d > hardEdge)
					falloff = 1.0 - (d - hardEdge) / softWidth;
				const double amount = std::clamp(opacity_ * falloff, 0.0, 1.0);
				uchar *px = row + x * 4;
				const int oldR = px[0];
				const int oldG = px[1];
				auto mixByte = [amount](int oldValue, int target) {
					return static_cast<uchar>(std::clamp(std::lround(oldValue + (target - oldValue) * amount), 0L, 255L));
				};
				if (mode_ == BrushMode::Erase) {
					px[0] = mixByte(oldR, 255);
					px[1] = mixByte(oldG, 0);
				} else if (mode_ == BrushMode::Protect) {
					px[0] = mixByte(oldR, 0);
					px[1] = mixByte(oldG, 255);
				} else {
					px[0] = mixByte(oldR, 0);
					px[1] = mixByte(oldG, 0);
				}
				px[2] = 0;
				px[3] = std::max(px[0], px[1]);
			}
		}
	}

	void commit(bool save)
	{
		if (auto state = state_.lock()) {
			state->setMask(working_);
			if (save)
				state->saveMask();
		}
		update();
	}

	std::weak_ptr<SmartMatteState> state_;
	QImage working_;
	std::vector<QImage> undo_;
	std::vector<QImage> redo_;
	BrushMode mode_ = BrushMode::Erase;
	int brushSize_ = 60;
	double hardness_ = 0.65;
	double opacity_ = 1.0;
	double zoom_ = 1.0;
	QPointF pan_;
	QPointF lastPaintPoint_;
	QPointF lastPanPoint_;
	QPointF cursorPos_;
	bool painting_ = false;
	bool panning_ = false;
	bool cursorInside_ = false;
};

class MaskEditorWidget final : public QWidget {
public:
	MaskEditorWidget()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);
		root->setSpacing(7);

		auto *sourceRow = new QHBoxLayout;
		sourceRow->addWidget(new QLabel(obs_module_text("Editor.Filter")));
		filterCombo_ = new QComboBox;
		filterCombo_->setMinimumWidth(220);
		sourceRow->addWidget(filterCombo_, 1);
		root->addLayout(sourceRow);

		auto *tools = new QGroupBox(obs_module_text("Editor.Tools"));
		auto *toolsLayout = new QGridLayout(tools);
		auto *erase = new QToolButton;
		auto *restore = new QToolButton;
		auto *protect = new QToolButton;
		erase->setText(obs_module_text("Editor.Erase"));
		restore->setText(obs_module_text("Editor.Restore"));
		protect->setText(obs_module_text("Editor.Protect"));
		for (auto *button : {erase, restore, protect}) {
			button->setCheckable(true);
			button->setToolButtonStyle(Qt::ToolButtonTextOnly);
		}
		erase->setChecked(true);
		auto *toolGroup = new QButtonGroup(this);
		toolGroup->setExclusive(true);
		toolGroup->addButton(erase, 0);
		toolGroup->addButton(restore, 1);
		toolGroup->addButton(protect, 2);
		toolsLayout->addWidget(erase, 0, 0);
		toolsLayout->addWidget(restore, 0, 1);
		toolsLayout->addWidget(protect, 0, 2);

		sizeSlider_ = makeSlider(1, 240, 60);
		hardnessSlider_ = makeSlider(0, 100, 65);
		opacitySlider_ = makeSlider(1, 100, 100);
		toolsLayout->addWidget(new QLabel(obs_module_text("Editor.Size")), 1, 0);
		toolsLayout->addWidget(sizeSlider_, 1, 1, 1, 2);
		toolsLayout->addWidget(new QLabel(obs_module_text("Editor.Hardness")), 2, 0);
		toolsLayout->addWidget(hardnessSlider_, 2, 1, 1, 2);
		toolsLayout->addWidget(new QLabel(obs_module_text("Editor.Opacity")), 3, 0);
		toolsLayout->addWidget(opacitySlider_, 3, 1, 1, 2);
		root->addWidget(tools);

		canvas_ = new MaskCanvas;
		root->addWidget(canvas_, 1);

		auto *actions = new QHBoxLayout;
		auto *undo = new QPushButton(obs_module_text("Editor.Undo"));
		auto *redo = new QPushButton(obs_module_text("Editor.Redo"));
		auto *clear = new QPushButton(obs_module_text("Editor.Clear"));
		auto *fit = new QPushButton(obs_module_text("Editor.Fit"));
		actions->addWidget(undo);
		actions->addWidget(redo);
		actions->addStretch(1);
		actions->addWidget(fit);
		actions->addWidget(clear);
		root->addLayout(actions);

		status_ = new QLabel(obs_module_text("Editor.Help"));
		status_->setWordWrap(true);
		root->addWidget(status_);

		QObject::connect(filterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
			[this](int index) { selectByIndex(index); });
		QObject::connect(toolGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, [this](int id) {
			canvas_->setBrushMode(id == 0 ? BrushMode::Erase : id == 1 ? BrushMode::Restore : BrushMode::Protect);
		});
		QObject::connect(sizeSlider_, &QSlider::valueChanged, canvas_, &MaskCanvas::setBrushSize);
		QObject::connect(hardnessSlider_, &QSlider::valueChanged, canvas_, &MaskCanvas::setHardness);
		QObject::connect(opacitySlider_, &QSlider::valueChanged, canvas_, &MaskCanvas::setOpacity);
		QObject::connect(undo, &QPushButton::clicked, canvas_, &MaskCanvas::undo);
		QObject::connect(redo, &QPushButton::clicked, canvas_, &MaskCanvas::redo);
		QObject::connect(clear, &QPushButton::clicked, canvas_, &MaskCanvas::clearMask);
		QObject::connect(fit, &QPushButton::clicked, canvas_, &MaskCanvas::fitView);

		refreshTimer_ = new QTimer(this);
		refreshTimer_->setInterval(700);
		QObject::connect(refreshTimer_, &QTimer::timeout, this, [this] { refreshFilters(); });
		refreshTimer_->start();
		refreshFilters();
	}

	void selectState(const std::shared_ptr<SmartMatteState> &state)
	{
		refreshFilters();
		if (!state)
			return;
		for (int i = 0; i < filterCombo_->count(); ++i) {
			if (filterCombo_->itemData(i).toString().toStdString() == state->instanceId) {
				filterCombo_->setCurrentIndex(i);
				canvas_->setState(state);
				break;
			}
		}
		if (auto *dock = qobject_cast<QDockWidget *>(parentWidget())) {
			dock->show();
			dock->raise();
		} else {
			show();
		}
	}

private:
	static QSlider *makeSlider(int minimum, int maximum, int value)
	{
		auto *slider = new QSlider(Qt::Horizontal);
		slider->setRange(minimum, maximum);
		slider->setValue(value);
		return slider;
	}

	void refreshFilters()
	{
		const auto states = registrySnapshot();
		const QString selectedId = filterCombo_->currentData().toString();
		bool changed = states.size() != static_cast<size_t>(filterCombo_->count());
		if (!changed) {
			for (int i = 0; i < filterCombo_->count(); ++i) {
				if (filterCombo_->itemData(i).toString().toStdString() != states[static_cast<size_t>(i)]->instanceId) {
					changed = true;
					break;
				}
			}
		}
		if (!changed)
			return;

		filterCombo_->blockSignals(true);
		filterCombo_->clear();
		for (const auto &state : states) {
			const QString shortId = QString::fromStdString(state->instanceId).left(8);
			const QString label = QString::fromStdString(state->displayName) + "  [" + shortId + "]";
			filterCombo_->addItem(label, QString::fromStdString(state->instanceId));
		}
		int restoreIndex = filterCombo_->findData(selectedId);
		if (restoreIndex < 0 && filterCombo_->count() > 0)
			restoreIndex = 0;
		filterCombo_->setCurrentIndex(restoreIndex);
		filterCombo_->blockSignals(false);
		selectByIndex(restoreIndex);
	}

	void selectByIndex(int index)
	{
		if (index < 0) {
			canvas_->setState({});
			return;
		}
		const std::string id = filterCombo_->itemData(index).toString().toStdString();
		for (const auto &state : registrySnapshot()) {
			if (state->instanceId == id) {
				canvas_->setState(state);
				return;
			}
		}
		canvas_->setState({});
	}

	QComboBox *filterCombo_ = nullptr;
	QSlider *sizeSlider_ = nullptr;
	QSlider *hardnessSlider_ = nullptr;
	QSlider *opacitySlider_ = nullptr;
	MaskCanvas *canvas_ = nullptr;
	QLabel *status_ = nullptr;
	QTimer *refreshTimer_ = nullptr;
};

