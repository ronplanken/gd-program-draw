// SPDX-License-Identifier: GPL-2.0-or-later
#include "canvas.hpp"
#include "ink-source.hpp"
#include "toolbar-ui.hpp"
#include <QToolBar>
#include <array>
#include <QToolButton>
#include <QMenu>
#include <QListWidget>
#include <QSlider>
#include <QToolTip>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QCheckBox>
#include <QActionGroup>
#include <QComboBox>
#include <QColorDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QPainter>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTabletEvent>
#include <QTimer>
#include <QVBoxLayout>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Draw on the original live monitor in normal or Studio Mode.";
}
MODULE_EXPORT const char *obs_module_name(void)
{
	return "GD Program Draw";
}
MODULE_EXPORT const char *obs_module_author(void)
{
	return "Ron Planken";
}

namespace program_draw {
// UI-only selection guide. It is never part of the libobs overlay texture.
class SelectionGuide final : public QWidget {
public:
	SelectionGuide(QWidget *parent, std::shared_ptr<Canvas> canvas) : QWidget(parent), canvas_(std::move(canvas))
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_TranslucentBackground);
		setFocusPolicy(Qt::NoFocus);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		// This transparent child sits above a native GPU surface. Explicitly
		// erase its previous pixels instead of relying on parent repainting.
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.fillRect(this->rect(), Qt::transparent);
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
		auto selection = canvas_->selection();
		if (selection.rect.isEmpty())
			return;
		auto size = canvas_->size();
		auto video = programVideoRect(parentWidget()->size(), parentWidget()->devicePixelRatioF(), size);
		p.setRenderHint(QPainter::Antialiasing);
		p.setClipRect(video);
		QRectF rect(video.x() + selection.rect.x() * video.width() / size.width(),
			    video.y() + selection.rect.y() * video.height() / size.height(),
			    selection.rect.width() * video.width() / size.width(),
			    selection.rect.height() * video.height() / size.height());
		p.setPen(QPen(Qt::black, 2));
		if (selection.ellipse)
			p.drawEllipse(rect);
		else
			p.drawRect(rect);
		p.setPen(QPen(Qt::white, 1, Qt::DashLine));
		if (selection.ellipse)
			p.drawEllipse(rect);
		else
			p.drawRect(rect);
	}

private:
	std::shared_ptr<Canvas> canvas_;
};
class Controller final : public QObject {
public:
	Controller(QMainWindow *main, std::shared_ptr<Canvas> canvas)
		: QObject(main),
		  main_(main),
		  canvas_(std::move(canvas))
	{
		for (int i = int(Tool::Highlighter); i < int(presets_.size()); ++i)
			presets_[i] = {QColor("#ffe34b"), 10, 1};
		presets_[int(Tool::Highlighter)] = {QColor("#ffe34b"), 128, .35};
		presets_[int(Tool::Spotlight)] = {Qt::black, 256, .6};
		connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
		timer_.start(500);
		obs_frontend_add_event_callback(frontendEvent, this);
	}
	~Controller() override { shutdown(); }
	void shutdown()
	{
		if (stopped_)
			return;
		stopped_ = true;
		timer_.stop();
		obs_frontend_remove_event_callback(frontendEvent, this);
		detach();
		releaseOutput();
	}

private:
	static void frontendEvent(obs_frontend_event event, void *data)
	{
		auto *self = static_cast<Controller *>(data);
		// OBS dispatches these frontend lifecycle events on its UI thread.
		if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
			self->shutdown();
			return;
		}
		if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING ||
		    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
			self->suspended_ = true;
			self->disarm();
			self->canvas_->clear();
			self->releaseOutput();
		}
		if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING ||
		    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
			self->suspended_ = false;
		if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED ||
		    event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED) {
			// OBS replaces the target monitor. Keep output/layers, but stop input
			// before attaching controls to the new live monitor.
			self->detach();
		}
		if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED && self->clearOnSceneChange_) {
			self->canvas_->clear();
			self->strokeActive_ = false;
		}
		// Studio Mode's widget may be created/destroyed just after the event.
		QTimer::singleShot(0, self, [self] { self->refresh(); });
	}
	void refresh()
	{
		if (stopped_ || suspended_ || !main_)
			return;
		const bool studio = obs_frontend_preview_program_mode_active();
		if (program_ && studio != studioTarget_)
			detach();
		obs_video_info info{};
		if (!obs_get_video_info(&info))
			return;
		const QSize size(info.base_width, info.base_height);
		if (canvas_->size() != size || canvas_->layers().empty()) {
			strokeActive_ = false;
			canvas_->resize(size);
			syncLayers();
		}
		if (source_ && channel_ >= 0) {
			auto *current = obs_get_output_source(uint32_t(channel_));
			const bool stillOurs = current == source_;
			obs_source_release(current);
			if (!stillOurs) {
				disarm();
				releaseOutput();
				setStatus("Overlay channel changed; enable Draw again");
			}
		}
		if (program_) {
			updateGuide();
			return;
		}
		auto *layout = main_->findChild<QLayout *>("previewLayout");
		if (!layout)
			return;
		// Studio Mode uses Program; normal mode uses the stock scene preview,
		// which renders the main output in that mode. Never target another dock.
		QList<QWidget *> candidates;
		for (int i = 0; i < layout->count(); ++i) {
			auto *container = layout->itemAt(i)->widget();
			if (!container)
				continue;
			for (auto *widget : container->findChildren<QWidget *>())
				if (QString::fromLatin1(widget->metaObject()->className()) ==
					    (studio ? "OBSQTDisplay" : "OBSBasicPreview") &&
				    (studio || widget->objectName() == "preview"))
					candidates.append(widget);
		}
		if (candidates.size() != 1)
			return;
		auto *display = candidates.first();
		auto *parent = display->parentWidget();
		auto *column = qobject_cast<QVBoxLayout *>(parent->layout());
		if (!column || (studio && column->indexOf(display) < 0))
			return;
		studioTarget_ = studio;
		program_ = display;
		oldFocusPolicy_ = display->focusPolicy();
		display->installEventFilter(this);
		display->setFocusPolicy(Qt::StrongFocus);
		guide_ = new SelectionGuide(display, canvas_);
		createToolbar(parent, column);
		blog(LOG_INFO, "[program-draw] Attached to original %s monitor (%s)",
		     studio ? "Program" : "normal preview", obs_get_version_string());
	}
	void updateGuide()
	{
		if (markerNumber_) {
			const QSignalBlocker block(markerNumber_);
			markerNumber_->setValue(canvas_->nextMarker());
		}
		const auto history = canvas_->historyAvailable();
		if (undoButton_)
			undoButton_->setEnabled(history.first);
		if (redoButton_)
			redoButton_->setEnabled(history.second);
		adaptToolbar();
		if (!guide_ || !program_)
			return;
		guide_->setGeometry(program_->rect());
		guide_->setVisible(armed_ && !canvas_->selection().rect.isEmpty());
		guide_->raise();
		guide_->update();
	}
	void queueLayerSync()
	{
		if (layerSyncPending_)
			return;
		layerSyncPending_ = true;
		QTimer::singleShot(0, this, [this] {
			layerSyncPending_ = false;
			layerMovePending_ = false;
			syncLayers();
		});
	}
	void syncLayers(bool rebuild = true)
	{
		if (!layerList_)
			return;
		const QSignalBlocker a(layerList_), c(layerOpacity_);
		const auto layers = canvas_->layers();
		const int active = canvas_->activeLayer();
		if (rebuild) {
			layerList_->clear();
			for (int i = int(layers.size()) - 1; i >= 0; --i) {
				const auto &layer = layers[i];
				auto *item = new QListWidgetItem(layerList_);
				item->setData(Qt::UserRole, i);
				item->setData(Qt::AccessibleTextRole, layer.name);
				item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
				if (layer.locked)
					item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
				item->setSizeHint(QSize(0, 38));
				auto *widget = new QWidget;
				auto *row = new QHBoxLayout(widget);
				row->setContentsMargins(10, 2, 4, 2);
				row->setSpacing(2);
				auto *label = new QLabel(layer.name);
				label->setObjectName("layerName");
				label->setAttribute(Qt::WA_TransparentForMouseEvents);
				label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
				row->addWidget(label, 1);
				auto addToggle = [&](int icon, const QString &name, bool checked) {
					auto *button = new QToolButton;
					button->setIcon(toolbarIcon(icon, layerList_->palette().color(QPalette::Text)));
					button->setIconSize(QSize(18, 18));
					button->setFixedSize(30, 30);
					button->setAutoRaise(true);
					button->setCheckable(true);
					button->setChecked(checked);
					button->setAccessibleName(name + " — " + layer.name);
					button->setToolTip(name + " — " + layer.name);
					button->setStyleSheet(
						"QToolButton { border: none; background: transparent; } QToolButton:hover { background: palette(mid); border-radius: 3px; }");
					row->addWidget(button);
					return button;
				};
				auto *eye = addToggle(layer.visible ? 30 : 31,
						      layer.visible ? "Hide layer" : "Show layer", layer.visible);
				eye->setObjectName("programDrawLayerEye");
				connect(eye, &QToolButton::clicked, this, [this, i](bool visible) {
					finishGesture();
					canvas_->selectLayer(i);
					canvas_->setLayerVisible(visible);
					queueLayerSync();
				});
				auto *lock = addToggle(layer.locked ? 32 : 33,
						       layer.locked ? "Unlock layer" : "Lock layer", layer.locked);
				lock->setObjectName("programDrawLayerLock");
				connect(lock, &QToolButton::clicked, this, [this, i](bool locked) {
					finishGesture();
					canvas_->selectLayer(i);
					canvas_->setLayerLocked(locked);
					queueLayerSync();
				});
				widget->setToolTip(layer.locked ? "Locked. Unlock to edit or reorder this layer."
								: "Drag to reorder. Top layer appears in front.");
				layerList_->setItemWidget(item, widget);
				if (i == active)
					layerList_->setCurrentItem(item);
			}
		}
		for (int row = 0; row < layerList_->count(); ++row) {
			auto *item = layerList_->item(row);
			if (auto *widget = layerList_->itemWidget(item)) {
				const bool selected = item->data(Qt::UserRole).toInt() == active;
				widget->findChild<QLabel *>("layerName")
					->setForegroundRole(selected ? QPalette::HighlightedText : QPalette::Text);
			}
		}
		if (!layers.empty()) {
			const bool editable = !layers[active].locked;
			layerOpacity_->setValue(qRound(layers[active].opacity * 100));
			layerOpacity_->setEnabled(editable);
			if (layerSlider_) {
				const QSignalBlocker block(layerSlider_);
				layerSlider_->setValue(layerOpacity_->value());
				layerSlider_->setEnabled(editable);
			}
			renameLayer_->setEnabled(editable);
			removeLayer_->setEnabled(editable && layers.size() > 1);
			addLayer_->setEnabled(layers.size() < 8);
			if (clearButton_)
				clearButton_->setToolTip(
					editable
						? "Clear " + layers[active].name +
							  " (Undo restores artwork). Locked layers are preserved."
						: "Active layer is locked. Unlock it to clear its artwork. The menu can clear other unlocked layers.");
			layersButton_->setText(QString("Layers · %1").arg(layers.size()));
			layersButton_->setToolTip("Active layer: " + layers[active].name +
						  (editable ? "" : " (locked)") +
						  "\nSelect, reorder and adjust drawing layers");
		}
		updateGuide();
	}
	void updateAppearance()
	{
		if (!appearanceButton_)
			return;
		const bool image = tool_ == Tool::Image || tool_ == Tool::Stamp;
		const bool selection = tool_ == Tool::SelectRectangle || tool_ == Tool::SelectEllipse ||
				       tool_ == Tool::None;
		const bool outline = tool_ == Tool::Pencil || tool_ == Tool::Brush || tool_ == Tool::Line ||
				     tool_ == Tool::RectangleOutline || tool_ == Tool::EllipseOutline ||
				     isCursorTool(tool_) || tool_ == Tool::Arrow || tool_ == Tool::CurvedArrow ||
				     tool_ == Tool::DashedRoute || tool_ == Tool::PlayerRing;
		appearanceButton_->setEnabled(!selection);
		appearanceButton_->setToolTip(QString("Appearance: %1 px, %2% opacity")
						      .arg(int(image ? imageWidth_ : width_))
						      .arg(qRound(opacity_ * 100)));
		appearanceButton_->setText(
			image     ? QString("%1 px · %2%").arg(int(imageWidth_)).arg(qRound(opacity_ * 100))
			: outline ? QString("%1 px · %2%").arg(int(width_)).arg(qRound(opacity_ * 100))
				  : QString("%1%").arg(qRound(opacity_ * 100)));
		if (widthRow_) {
			widthRow_->setVisible(outline);
			if (auto *label = widthRow_->findChild<QLabel *>())
				label->setText(isCursorTool(tool_) ? "Diameter" : "Width");
		}
		if (curveRow_)
			curveRow_->setVisible(tool_ == Tool::CurvedArrow);
		if (markerGroup_)
			markerGroup_->setVisible(tool_ == Tool::NumberMarker);
		if (outlineCheck_)
			outlineCheck_->setVisible(tool_ == Tool::Arrow || tool_ == Tool::CurvedArrow ||
						  tool_ == Tool::DashedRoute || tool_ == Tool::PlayerRing ||
						  tool_ == Tool::NumberMarker);
		if (appearanceHint_) {
			QString hint = "Changes apply to your next mark.";
			if (tool_ == Tool::Highlighter)
				hint = "Hold the mouse button to show a circular highlight at the cursor. Move to follow; release to hide. Diameter and opacity control its appearance.";
			if (tool_ == Tool::Arrow)
				hint = "Drag from tail to tip. Hold Shift for 45° angles.";
			if (tool_ == Tool::CurvedArrow)
				hint = "Drag tail to tip. Bend changes the arc; negative values flip it.";
			if (tool_ == Tool::DashedRoute)
				hint = "Draw a movement route. An arrow points along its final segment.";
			if (tool_ == Tool::Spotlight)
				hint = "Hold the mouse button to focus a circle on the cursor and dim the surrounding video. Move to follow; release to hide. Diameter sets the circle size and opacity sets the dimming.";
			if (tool_ == Tool::PlayerRing)
				hint = "Click for a ground ring, or drag to frame a player or zone.";
			if (tool_ == Tool::NumberMarker)
				hint = "Click to place a numbered badge. Counts up to 99; Undo restores numbering.";
			appearanceHint_->setText(hint);
		}
		if (assetGroup_)
			assetGroup_->setVisible(image);
		if (assetHint_)
			assetHint_->setText(
				asset_.isNull()
					? "No image selected"
					: QString("%1 × %2 px image ready").arg(asset_.width()).arg(asset_.height()));
		if (assetButton_)
			assetButton_->setText(asset_.isNull() ? "Choose image…" : "Replace image…");
		colorButton_->setEnabled(!image && !selection && tool_ != Tool::Spotlight);
	}
	void finishGesture()
	{
		canvas_->finish();
		strokeActive_ = false;
		updateGuide();
	}
	void syncToolPicker()
	{
		if (!toolPickerButton_)
			return;
		const int i = int(tool_);
		const bool shape = isShapeTool(tool_);
		const QString name = shape ? "Shape" : ToolNames[i];
		toolPickerButton_->setIcon(toolbarIcon(shape    ? int(Tool::RectangleOutline)
						       : i < 12 ? i
								: i + 9,
						       toolPickerButton_->palette().color(QPalette::ButtonText)));
		toolPickerButton_->setText(name);
		const QString category = i < 12 ? "Draw" : "Analysis";
		toolPickerButton_->setAccessibleName("Tools: " + name + " (" + category + ")");
		toolPickerButton_->setToolTip("Choose a Draw or Analysis tool. Current: " + name);
		for (int j = 0; j < int(toolActions_.size()); ++j)
			if (toolActions_[j])
				toolActions_[j]->setChecked(j == (shape ? int(ShapeTools.front()) : i));
		if (shapeControlAction_) {
			shapeControlAction_->setVisible(shape);
			shapeButton_->setIcon(
				toolbarIcon(int(shapeTool_), shapeButton_->palette().color(QPalette::ButtonText)));
			shapeButton_->setText(ToolNames[int(shapeTool_)]);
			shapeButton_->setAccessibleName("Shape type: " + ToolNames[int(shapeTool_)]);
			shapeButton_->setToolTip("Choose shape type. Current: " + ToolNames[int(shapeTool_)]);
			for (auto *action : shapeButton_->menu()->actions())
				action->setChecked(action->data().toInt() == int(shapeTool_));
		}
	}
	void selectTool(Tool selected)
	{
		finishGesture();
		presets_[int(tool_)] = {color_, width_, opacity_};
		tool_ = selected;
		if (isShapeTool(selected))
			shapeTool_ = selected;
		const auto preset = presets_[int(tool_)];
		color_ = preset.color;
		width_ = preset.width;
		opacity_ = preset.opacity;
		if (widthSpin_)
			widthSpin_->setValue(int(width_));
		if (opacitySpin_)
			opacitySpin_->setValue(qRound(opacity_ * 100));
		updateColor();
		updateAppearance();
		syncToolPicker();
		if (program_) {
			if (armed_ && tool_ != Tool::None)
				program_->setCursor(Qt::CrossCursor);
			else
				program_->unsetCursor();
		}
		if (tool_ == Tool::SelectRectangle || tool_ == Tool::SelectEllipse)
			setStatus(
				"Drag to select active-layer pixels, then drag inside to move. More contains selection actions.");
		else if (tool_ == Tool::Stamp || tool_ == Tool::Image)
			setStatus(asset_.isNull() ? "Open Appearance to choose an image."
						  : "Image: drag to size. Stamp: click to repeat.");
		else
			setStatus("Choose Draw to mark the live video. Shift constrains shapes.");
		adaptToolbar();
	}
	void loadImage()
	{
		finishGesture();
		const QString path = QFileDialog::getOpenFileName(main_, "Choose image or stamp", QString(),
								  "Images (*.png *.jpg *.jpeg *.bmp *.webp)");
		if (path.isEmpty())
			return;
		QImageReader reader(path);
		reader.setAutoTransform(true);
		// Limit decoding to a practical POC asset size, including very large photos.
		QSize size = reader.size();
		if (!size.isValid()) {
			setStatus("Could not read this image");
			return;
		}
		if (size.width() > 4096 || size.height() > 4096)
			reader.setScaledSize(size.scaled(4096, 4096, Qt::KeepAspectRatio));
		auto image = reader.read();
		if (image.isNull()) {
			setStatus("Could not load image: " + reader.errorString());
			return;
		}
		asset_ = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
		assetButton_->setToolTip(path);
		assetButton_->setText("Image loaded…");
		setStatus(QFileInfo(path).fileName() + " loaded. Image: drag to size. Stamp: click to repeat.");
		if (tool_ != Tool::Image && tool_ != Tool::Stamp)
			selectTool(Tool::Image);
		updateAppearance();
	}
	void createToolbar(QWidget *parent, QVBoxLayout *column)
	{
		toolbarHost_ = new QWidget(parent);
		toolbarHost_->setObjectName("programDrawToolbarHost");
		toolbarHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		toolbarHost_->setFixedHeight(46);
		toolbarHost_->installEventFilter(this);
		auto makeBar = [this](const char *name) {
			auto *bar = new QToolBar(toolbarHost_);
			bar->setObjectName(name);
			bar->setMovable(false);
			bar->setFloatable(false);
			bar->setIconSize(QSize(20, 20));
			bar->setMinimumWidth(0);
			bar->setContentsMargins(0, 0, 0, 0);
			bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
			bar->setStyleSheet(
				"QToolBar { spacing: 4px; padding: 5px 0px; margin: 0px; border: none; } QToolBar QToolButton { padding: 5px 7px; min-height: 24px; border-radius: 4px; } QToolBar QToolButton:checked { background: #a92d40; color: white; }");
			return bar;
		};
		auto *bar = makeBar("programDrawToolbar");
		toolbar_ = bar;
		const QColor ink = bar->palette().color(QPalette::ButtonText);
		auto toolButton = [&](QString label, const char *name, int icon, bool text = true,
				      bool dropdown = false) {
			QToolButton *button = dropdown ? new DropdownToolButton(bar) : new QToolButton(bar);
			button->setObjectName(name);
			button->setText(label);
			button->setAccessibleName(label);
			button->setToolTip(label);
			if (icon >= 0)
				button->setIcon(toolbarIcon(icon, ink));
			button->setToolButtonStyle(text ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
			bar->addWidget(button);
			return button;
		};
		auto popupButton = [&](QHBoxLayout *row, QString label, const char *name, auto action) {
			auto *button = new QPushButton(label);
			button->setObjectName(name);
			row->addWidget(button);
			connect(button, &QPushButton::clicked, this, action);
			return button;
		};
		auto sliderRow = [&](QVBoxLayout *layout, QString label, const char *name, int min, int max, int value,
				     QString suffix, auto change, QSlider **sliderOut = nullptr) {
			auto *container = new QWidget;
			auto *row = new QHBoxLayout(container);
			row->setContentsMargins(0, 0, 0, 0);
			row->setSpacing(10);
			auto *caption = new QLabel(label);
			caption->setMinimumWidth(62);
			row->addWidget(caption);
			auto *slider = new QSlider(Qt::Horizontal);
			slider->setRange(min, max);
			slider->setValue(value);
			slider->setTracking(false);
			slider->setAccessibleName(label);
			row->addWidget(slider, 1);
			auto *spin = new QSpinBox;
			spin->setObjectName(name);
			spin->setAccessibleName(label);
			spin->setRange(min, max);
			spin->setValue(value);
			spin->setSuffix(suffix);
			spin->setFixedWidth(90);
			row->addWidget(spin);
			caption->setBuddy(spin);
			connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
			connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
			connect(spin, &QSpinBox::valueChanged, this, change);
			layout->addWidget(container);
			if (sliderOut)
				*sliderOut = slider;
			return spin;
		};
		drawButton_ = toolButton("Draw", "programDrawToggle", 29);
		drawButton_->setCheckable(true);
		drawButton_->setToolTip("Enable drawing on the live output");
		connect(drawButton_, &QToolButton::toggled, this, [this](bool enabled) {
			if (enabled && !acquireOutput()) {
				const QString error = statusText_;
				disarm();
				setStatus(error);
				return;
			}
			armed_ = enabled;
			finishGesture();
			drawButton_->setText(enabled ? "LIVE" : "Draw");
			drawButton_->setAccessibleName(enabled ? "Drawing live — click to pause" : "Enable drawing");
			if (program_) {
				if (enabled && tool_ != Tool::None) {
					program_->setCursor(Qt::CrossCursor);
					program_->setFocus();
				} else
					program_->unsetCursor();
			}
			updateGuide();
			setStatus(enabled ? "Drawing is LIVE. Shift constrains shapes; Escape stops drawing."
					  : "Drawing paused. Existing artwork remains on output.");
		});
		bar->addSeparator();
		toolPickerButton_ = toolButton("Tools", "programDrawTools", 1, true, true);
		toolsPanel_ = makePopover(bar, "Tools");
		toolsPanel_->setFixedWidth(480);
		toolsPanel_->setStyleSheet(
			toolsPanel_->styleSheet() +
			" QFrame#programDrawPopover QPushButton { text-align: left; padding: 5px 9px; min-height: 24px; border: 1px solid transparent; border-radius: 4px; } QFrame#programDrawPopover QPushButton:hover { background: palette(mid); } QFrame#programDrawPopover QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); } QFrame#programDrawPopover QPushButton:focus { border: 1px solid palette(highlight); }");
		auto *columns = new QHBoxLayout;
		columns->setSpacing(16);
		qobject_cast<QVBoxLayout *>(toolsPanel_->layout())->addLayout(columns);
		auto *toolActions = new QActionGroup(bar);
		toolActions->setExclusive(true);
		auto addColumn = [&](QString title, int first, int last) {
			auto *column = new QVBoxLayout;
			column->setSpacing(3);
			columns->addLayout(column, 1);
			auto *heading = new QLabel(title);
			auto font = heading->font();
			font.setBold(true);
			heading->setFont(font);
			heading->setContentsMargins(9, 0, 0, 6);
			column->addWidget(heading);
			for (int i = first; i < last; ++i) {
				const bool shape = isShapeTool(Tool(i));
				if (shape && Tool(i) != ShapeTools.front())
					continue;
				const QString name = shape ? "Shape" : ToolNames[i];
				auto *action = new QAction(toolbarIcon(shape    ? int(Tool::RectangleOutline)
								       : i < 12 ? i
										: i + 9,
								       ink),
							   name, toolActions);
				action->setCheckable(true);
				toolActions->addAction(action);
				toolActions_[i] = action;
				auto *button = new QPushButton(action->icon(), name, toolsPanel_);
				button->setObjectName(QString("programDrawTool%1").arg(i));
				button->setAccessibleName(title + ": " + name);
				button->setCheckable(true);
				button->setIconSize(QSize(20, 20));
				column->addWidget(button);
				connect(action, &QAction::changed, button,
					[action, button] { button->setChecked(action->isChecked()); });
				connect(button, &QPushButton::clicked, action, [action, button] {
					action->trigger();
					button->setChecked(action->isChecked());
				});
				connect(action, &QAction::triggered, this, [this, i] {
					toolsPanel_->hide();
					selectTool(isShapeTool(Tool(i)) ? shapeTool_ : Tool(i));
				});
			}
			column->addStretch();
		};
		addColumn("Draw", 0, 12);
		addColumn("Analysis", 12, ToolNames.size());
		connect(toolPickerButton_, &QToolButton::clicked, this, [this] {
			finishGesture();
			appearancePanel_->hide();
			layersPanel_->hide();
			syncToolPicker();
			showPopover(toolsPanel_, toolPickerButton_);
		});
		syncToolPicker();
		colorButton_ = toolButton("Colour", "programDrawColor", -1, false);
		connect(colorButton_, &QToolButton::clicked, this, [this] {
			finishGesture();
			auto chosen = QColorDialog::getColor(color_, main_, "Drawing colour");
			if (chosen.isValid()) {
				color_ = chosen;
				updateColor();
			}
		});
		updateColor();
		shapeButton_ = toolButton("Shape type", "programDrawShape", int(shapeTool_), true, true);
		shapeControlAction_ = bar->actions().last();
		shapeButton_->setPopupMode(QToolButton::InstantPopup);
		auto *shapeMenu = new QMenu(shapeButton_);
		auto *shapeActions = new QActionGroup(shapeMenu);
		shapeActions->setExclusive(true);
		for (const auto shape : ShapeTools) {
			auto *action = shapeMenu->addAction(toolbarIcon(int(shape), ink), ToolNames[int(shape)]);
			action->setCheckable(true);
			action->setData(int(shape));
			shapeActions->addAction(action);
			connect(action, &QAction::triggered, this, [this, shape] { selectTool(shape); });
		}
		shapeButton_->setMenu(shapeMenu);
		connect(shapeMenu, &QMenu::aboutToShow, this, [this] {
			finishGesture();
			toolsPanel_->hide();
			appearancePanel_->hide();
			layersPanel_->hide();
		});
		appearanceButton_ = toolButton("Appearance", "programDrawAppearance", 15);
		appearanceButton_->setAccessibleName("Appearance: width, opacity and image");
		appearancePanel_ = makePopover(bar, "Appearance");
		appearancePanel_->setFixedWidth(360);
		auto *settings = qobject_cast<QVBoxLayout *>(appearancePanel_->layout());
		auto *width =
			sliderRow(settings, "Width", "programDrawWidth", 1, 256, int(width_), " px", [this](int v) {
				width_ = v;
				updateAppearance();
			});
		widthRow_ = width->parentWidget();
		widthSpin_ = width;
		opacitySpin_ = sliderRow(settings, "Opacity", "programDrawOpacity", 0, 100, qRound(opacity_ * 100), "%",
					 [this](int v) {
						 opacity_ = v / 100.0;
						 updateAppearance();
					 });
		auto *curve = sliderRow(settings, "Bend", "programDrawCurve", -100, 100, int(curvature_), "%",
					[this](int value) { curvature_ = value; });
		curveRow_ = curve->parentWidget();
		markerGroup_ = new QWidget;
		auto *markers = new QVBoxLayout(markerGroup_);
		markers->setContentsMargins(0, 0, 0, 0);
		markers->setSpacing(10);
		settings->addWidget(markerGroup_);
		sliderRow(markers, "Badge size", "programDrawMarkerSize", 24, 256, int(markerSize_), " px",
			  [this](int value) { markerSize_ = value; });
		auto *numberRow = new QHBoxLayout;
		numberRow->addWidget(new QLabel("Next number"));
		markerNumber_ = new QSpinBox;
		markerNumber_->setObjectName("programDrawNextNumber");
		markerNumber_->setRange(1, 99);
		markerNumber_->setValue(canvas_->nextMarker());
		numberRow->addWidget(markerNumber_);
		markers->addLayout(numberRow);
		connect(markerNumber_, &QSpinBox::valueChanged, this, [this](int value) {
			canvas_->setNextMarker(value);
			updateGuide();
		});
		outlineCheck_ = new QCheckBox("Contrast outline");
		outlineCheck_->setChecked(contrastOutline_);
		settings->addWidget(outlineCheck_);
		connect(outlineCheck_, &QCheckBox::toggled, this, [this](bool value) { contrastOutline_ = value; });
		assetGroup_ = new QWidget;
		auto *assets = new QVBoxLayout(assetGroup_);
		assets->setContentsMargins(0, 6, 0, 0);
		assets->setSpacing(10);
		settings->addWidget(assetGroup_);
		assetHint_ = new QLabel;
		assets->addWidget(assetHint_);
		auto *assetRow = new QHBoxLayout;
		assets->addLayout(assetRow);
		assetButton_ = popupButton(assetRow, "Choose image…", "programDrawImage", [this] {
			appearancePanel_->hide();
			loadImage();
		});
		stampWidth_ = sliderRow(assets, "Size", "programDrawStampWidth", 4, 4096, int(imageWidth_), " px",
					[this](int v) {
						imageWidth_ = v;
						updateAppearance();
					});
		auto *hint = new QLabel("Changes apply to your next mark.");
		appearanceHint_ = hint;
		hint->setWordWrap(true);
		settings->addWidget(hint);
		connect(appearanceButton_, &QToolButton::clicked, this, [this] {
			finishGesture();
			updateAppearance();
			toolsPanel_->hide();
			layersPanel_->hide();
			showPopover(appearancePanel_, appearanceButton_);
		});
		bar = makeBar("programDrawSecondaryToolbar");
		secondaryToolbar_ = bar;
		auto *undo = toolButton("Undo", "programDrawUndo", 12, false);
		connect(undo, &QToolButton::clicked, this, [this] {
			finishGesture();
			canvas_->undo();
			syncLayers();
		});
		undo->setToolTip("Undo drawing or layer change (⌘Z)");
		undoButton_ = undo;
		auto *redo = toolButton("Redo", "programDrawRedo", 13, false);
		connect(redo, &QToolButton::clicked, this, [this] {
			finishGesture();
			canvas_->redo();
			syncLayers();
		});
		redo->setToolTip("Redo (⇧⌘Z)");
		redoButton_ = redo;
		clearButton_ = toolButton("Clear", "programDrawClear", 28, false, true);
		clearButton_->setPopupMode(QToolButton::MenuButtonPopup);
		clearButton_->setToolTip(
			"Clear the active layer. Use the arrow for more clearing options. Undo restores cleared artwork.");
		clearButton_->setStyleSheet(
			clearButton_->styleSheet() +
			" QToolButton#programDrawClear::menu-button { width: 26px; border-left: 1px solid palette(mid); } QToolButton#programDrawClear::menu-arrow { image: none; width: 0px; height: 0px; }");
		connect(clearButton_, &QToolButton::clicked, this, [this] {
			finishGesture();
			canvas_->clearLayer();
			syncLayers();
		});
		auto *clearMenu = new QMenu(clearButton_);
		clearButton_->setMenu(clearMenu);
		auto *clearLayer = clearMenu->addAction("Clear active layer");
		connect(clearLayer, &QAction::triggered, this, [this] {
			finishGesture();
			canvas_->clearLayer();
			syncLayers();
		});
		auto *clearAll = clearMenu->addAction("Clear all layers");
		connect(clearAll, &QAction::triggered, this, [this] {
			canvas_->clearAll();
			strokeActive_ = false;
			syncLayers();
		});
		clearMenu->addSeparator();
		auto *clearSelected = clearMenu->addAction("Clear selected pixels");
		connect(clearSelected, &QAction::triggered, this, [this] {
			finishGesture();
			canvas_->deleteSelection();
			updateGuide();
		});
		auto *clearSpotlight = clearMenu->addAction("Clear spotlight on active layer");
		connect(clearSpotlight, &QAction::triggered, this, [this] {
			canvas_->clearSpotlight();
			updateGuide();
		});
		connect(clearMenu, &QMenu::aboutToShow, this,
			[this, clearSelected, clearLayer, clearAll, clearSpotlight] {
				clearSelected->setEnabled(!canvas_->selection().rect.isEmpty());
				const auto layers = canvas_->layers();
				const int active = canvas_->activeLayer();
				if (!layers.empty()) {
					const bool editable = !layers[active].locked;
					clearLayer->setText("Clear active layer: " + layers[active].name);
					clearLayer->setEnabled(editable);
					clearSelected->setEnabled(editable && !canvas_->selection().rect.isEmpty());
					clearSpotlight->setEnabled(editable);
					const bool anyLocked =
						std::any_of(layers.begin(), layers.end(),
							    [](const Canvas::LayerInfo &l) { return l.locked; });
					clearAll->setText(anyLocked ? "Clear all unlocked layers" : "Clear all layers");
					clearAll->setEnabled(
						std::any_of(layers.begin(), layers.end(),
							    [](const Canvas::LayerInfo &l) { return !l.locked; }));
				}
			});
		clearMenu->addSeparator();
		auto *autoClear = clearMenu->addAction("Clear when scene changes");
		autoClear->setCheckable(true);
		autoClear->setChecked(clearOnSceneChange_);
		connect(autoClear, &QAction::toggled, this, [this](bool value) { clearOnSceneChange_ = value; });
		bar->addSeparator();
		layersButton_ = toolButton("Layers", "programDrawLayers", 14);
		layersPanel_ = makePopover(bar, "Drawing layers");
		layersPanel_->setFixedWidth(360);
		auto *layers = qobject_cast<QVBoxLayout *>(layersPanel_->layout());
		auto *layerHint = new QLabel("Drag to reorder. Top layer appears in front.");
		layers->addWidget(layerHint);
		layerList_ = new QListWidget;
		layerList_->setObjectName("programDrawLayerList");
		layerList_->setAccessibleName("Drawing layers, front to back");
		layerList_->setFixedHeight(180);
		layerList_->setSelectionMode(QAbstractItemView::SingleSelection);
		layerList_->setDragDropMode(QAbstractItemView::InternalMove);
		layerList_->setDefaultDropAction(Qt::MoveAction);
		layerList_->setDragDropOverwriteMode(false);
		layerList_->setDropIndicatorShown(true);
		layerList_->setStyleSheet(
			"QListWidget::item { padding: 0px; } QListWidget::item:selected { background: palette(highlight); } QListWidget::item:hover:!selected { background: palette(alternate-base); }");
		connect(layerList_->model(), &QAbstractItemModel::rowsAboutToBeMoved, this,
			[this] { layerMovePending_ = true; });
		connect(layerList_->model(), &QAbstractItemModel::rowsMoved, this,
			[this](const QModelIndex &, int start, int, const QModelIndex &, int destination) {
				const int toRow = destination > start ? destination - 1 : destination;
				finishGesture();
				canvas_->selectLayer(layerList_->count() - 1 - start);
				canvas_->moveLayerTo(layerList_->count() - 1 - toRow);
				queueLayerSync();
			});
		layers->addWidget(layerList_);
		connect(layerList_, &QListWidget::currentRowChanged, this, [this](int row) {
			if (row < 0 || layerMovePending_)
				return;
			auto *item = layerList_->item(row);
			finishGesture();
			canvas_->selectLayer(item->data(Qt::UserRole).toInt());
			syncLayers(false);
		});
		auto *row = new QHBoxLayout;
		layers->addLayout(row);
		addLayer_ = popupButton(row, "+", "programDrawAddLayer", [this] {
			finishGesture();
			canvas_->addLayer();
			syncLayers();
		});
		addLayer_->setToolTip("Add layer");
		removeLayer_ = popupButton(row, "−", "programDrawRemoveLayer", [this] {
			finishGesture();
			canvas_->removeLayer();
			syncLayers();
		});
		removeLayer_->setToolTip("Remove layer (Undo restores it)");
		int layerIcon = 17;
		for (auto *b : {addLayer_.data(), removeLayer_.data()}) {
			b->setText({});
			b->setIcon(toolbarIcon(layerIcon++, ink));
			b->setIconSize(QSize(18, 18));
			b->setStyleSheet("padding: 0px; min-width: 32px; max-width: 32px; min-height: 30px;");
			b->setAccessibleName(b->toolTip());
		}
		renameLayer_ = popupButton(row, "Rename…", "programDrawRenameLayer", [this] {
			auto list = canvas_->layers();
			if (list.empty())
				return;
			layersPanel_->hide();
			bool ok = false;
			auto name = QInputDialog::getText(main_, "Rename layer", "Layer name", QLineEdit::Normal,
							  list[canvas_->activeLayer()].name, &ok);
			if (ok)
				canvas_->renameLayer(name);
			syncLayers();
		});
		QSlider *layerSlider = nullptr;
		layerOpacity_ = sliderRow(
			layers, "Opacity", "programDrawLayerOpacity", 0, 100, 100, "%",
			[this](int v) {
				finishGesture();
				canvas_->setLayerOpacity(v / 100.0);
			},
			&layerSlider);
		layerSlider_ = layerSlider;
		connect(layersButton_, &QToolButton::clicked, this, [this] {
			finishGesture();
			syncLayers();
			toolsPanel_->hide();
			appearancePanel_->hide();
			showPopover(layersPanel_, layersButton_);
		});
		auto *more = toolButton("More", "programDrawMore", 16, false);
		moreButton_ = more;
		more->setPopupMode(QToolButton::InstantPopup);
		more->setStyleSheet(
			"QToolButton#programDrawMore::menu-indicator { image: none; width: 0px; height: 0px; }");
		auto *menu = new QMenu(more);
		more->setMenu(menu);
		auto *import = menu->addAction("Import image…");
		connect(import, &QAction::triggered, this, [this] { loadImage(); });
		menu->addSeparator();
		auto *stamp = menu->addAction("Selection to stamp");
		connect(stamp, &QAction::triggered, this, [this] {
			finishGesture();
			auto image = canvas_->selectionImage();
			if (image.isNull())
				return;
			asset_ = image;
			stampWidth_->setValue(image.width());
			selectTool(Tool::Stamp);
			updateAppearance();
		});
		auto *deselect = menu->addAction("Deselect");
		connect(deselect, &QAction::triggered, this, [this] {
			canvas_->deselect();
			updateGuide();
		});
		connect(menu, &QMenu::aboutToShow, this, [this, stamp, deselect] {
			const bool selected = !canvas_->selection().rect.isEmpty();
			stamp->setEnabled(selected);
			deselect->setEnabled(selected);
		});
		menu->addSeparator();
		auto *resetNumbers = menu->addAction("Restart marker numbering at 1");
		connect(resetNumbers, &QAction::triggered, this, [this] {
			canvas_->setNextMarker(1);
			updateGuide();
		});
		menu->addSeparator();
		auto *help = menu->addAction("Drawing tips…");
		connect(help, &QAction::triggered, this, [more] {
			QToolTip::showText(
				more->mapToGlobal(QPoint(0, 0)),
				"Draw marks the live output.\nShift: constrain shapes · Escape: pause\nSelect pixels to move, delete or reuse as a stamp.\nArtwork is session-only; closing OBS clears it.",
				more);
		});
		syncToolPicker();
		syncLayers();
		updateAppearance();
		QTimer::singleShot(0, bar, [this] { adaptToolbar(); });
		if (studioTarget_)
			column->insertWidget(column->indexOf(program_) + 1, toolbarHost_);
		else {
			column->addWidget(toolbarHost_);
			if (auto *scaling = main_->findChild<QComboBox *>("previewScalingMode"))
				connect(scaling, &QComboBox::currentIndexChanged, bar, [this](int index) {
					if (armed_ && index != 0) {
						disarm();
						setStatus("Preview zoom changed. Enable Draw again to fit the canvas.");
					}
				});
		}
	}
	void adaptToolbar()
	{
		if (arrangingToolbar_ || !toolbarHost_ || !toolbar_ || !secondaryToolbar_ || !program_ ||
		    !appearanceButton_ || !layersButton_ || !toolPickerButton_ || !moreButton_)
			return;
		arrangingToolbar_ = true;
		const auto video = programVideoRect(program_->size(), program_->devicePixelRatioF(), canvas_->size());
		// Program is a native window; global mapping includes its platform offset.
		const QPoint origin = toolbarHost_->mapFromGlobal(program_->mapToGlobal(QPoint(0, 0)));
		const double offset = origin.x();
		const int available = toolbarHost_->width();
		const int left = std::clamp(qRound(offset + video.left()), 0, available);
		const int right = std::clamp(qRound(offset + video.right()), left, available);
		const int width = right - left;
		appearanceButton_->setToolButtonStyle(width < 800 ? Qt::ToolButtonIconOnly
								  : Qt::ToolButtonTextBesideIcon);
		layersButton_->setToolButtonStyle(width < 650 ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
		toolPickerButton_->setToolButtonStyle(width < 520 ? Qt::ToolButtonIconOnly
								  : Qt::ToolButtonTextBesideIcon);
		toolPickerButton_->setFixedWidth(width < 520          ? 56
						 : isShapeTool(tool_) ? 110
						 : width < 650        ? 150
						 : width < 800        ? 170
								      : 200);
		if (shapeButton_)
			shapeButton_->setToolButtonStyle(width < 1000 ? Qt::ToolButtonIconOnly
								      : Qt::ToolButtonTextBesideIcon);
		toolbar_->ensurePolished();
		secondaryToolbar_->ensurePolished();
		const QSize primary = toolbar_->sizeHint();
		const QSize secondary = secondaryToolbar_->sizeHint();
		const int height = std::max({38, primary.height(), secondary.height()});
		toolbarHost_->setFixedHeight(height);
		const int rightWidth = std::min(width, secondary.width());
		const int leftWidth = std::min(primary.width(), std::max(0, width - rightWidth - 16));
		// Anchor each group to the actual picture edge, including pillarboxing
		// and the normal preview's scrollbar offset. Extra space stays between them.
		toolbar_->resize(leftWidth, height);
		secondaryToolbar_->resize(rightWidth, height);
		toolbar_->layout()->activate();
		secondaryToolbar_->layout()->activate();
		// Align the buttons themselves, independent of the theme's toolbar insets.
		toolbar_->move(left - drawButton_->mapTo(toolbar_, QPoint(0, 0)).x(), 0);
		secondaryToolbar_->move(
			right - moreButton_->mapTo(secondaryToolbar_, QPoint(moreButton_->width(), 0)).x(), 0);
		arrangingToolbar_ = false;
	}
	void updateColor()
	{
		if (!colorButton_)
			return;
		QPixmap swatch(40, 40);
		swatch.fill(Qt::transparent);
		swatch.setDevicePixelRatio(2);
		QPainter p(&swatch);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(colorButton_->palette().color(QPalette::ButtonText), 1));
		p.setBrush(color_);
		p.drawRoundedRect(QRectF(2, 2, 16, 16), 4, 4);
		p.end();
		colorButton_->setIcon(QIcon(swatch));
		colorButton_->setToolTip("Colour: " + color_.name());
	}
	void setStatus(const QString &text)
	{
		statusText_ = text;
		if (drawButton_)
			drawButton_->setToolTip(text);
		if (toolbar_)
			toolbar_->setToolTip(text);
		// Feedback appears only when needed, never as a permanent extra row.
		if (toolbar_ &&
		    (text.contains("Could not") || text.contains("needs") || text.contains("No unused") ||
		     text.contains("supports SDR") || text.contains("zoom changed") || text.contains("No drawing")))
			QToolTip::showText(toolbar_->mapToGlobal(QPoint(8, 0)), text, toolbar_);
	}
	bool acquireOutput()
	{
		if (suspended_ || stopped_ || !program_)
			return false;
		obs_video_info info{};
		if (!obs_get_video_info(&info))
			return false;
		if (info.colorspace != VIDEO_CS_709 && info.colorspace != VIDEO_CS_SRGB &&
		    info.colorspace != VIDEO_CS_601) {
			setStatus("This proof of concept supports SDR video only");
			return false;
		}
		if (!studioTarget_) {
			// Use Qt's checked meta-call, not a cast into OBS's private C++ class.
			// Fit mode uses the same pixel geometry as the Program monitor.
			if (!QMetaObject::invokeMethod(main_, "setPreviewScalingWindow", Qt::DirectConnection)) {
				setStatus("Could not fit the normal preview; this OBS version needs an adapter update");
				return false;
			}
			obs_frontend_set_preview_enabled(true);
		}
		canvas_->resize(QSize(info.base_width, info.base_height));
		if (source_)
			return true;
		// Channel 0 is Program and 1–5 are normally audio. Claim an unused high
		// channel; never replace another plugin's or OBS's output source.
		for (int i = MAX_CHANNELS - 1; i >= 6; --i) {
			auto *existing = obs_get_output_source(uint32_t(i));
			const bool free = existing == nullptr;
			obs_source_release(existing);
			if (!free)
				continue;
			source_ = obs_source_create_private(SourceId, "Program Draw POC overlay", nullptr);
			if (!source_) {
				setStatus("Could not create drawing overlay");
				return false;
			}
			channel_ = i;
			obs_set_output_source(uint32_t(i), source_);
			blog(LOG_INFO, "[program-draw] Live overlay on main output channel %d", i);
			return true;
		}
		setStatus("No unused output channel is available");
		return false;
	}
	void releaseOutput()
	{
		if (!source_)
			return;
		if (channel_ >= 0) {
			auto *current = obs_get_output_source(uint32_t(channel_));
			if (current == source_)
				obs_set_output_source(uint32_t(channel_), nullptr);
			obs_source_release(current);
		}
		obs_source_release(source_);
		source_ = nullptr;
		channel_ = -1;
	}
	void disarm()
	{
		armed_ = false;
		strokeActive_ = false;
		canvas_->finish();
		if (drawButton_)
			drawButton_->setChecked(false);
		if (program_)
			program_->unsetCursor();
		updateGuide();
	}
	void detach()
	{
		disarm();
		if (program_) {
			program_->removeEventFilter(this);
			program_->setFocusPolicy(oldFocusPolicy_);
		}
		program_ = nullptr;
		delete guide_.data();
		guide_ = nullptr;
		delete toolbarHost_.data();
		toolbarHost_ = nullptr;
		toolbar_ = nullptr;
		secondaryToolbar_ = nullptr;
		drawButton_ = nullptr;
	}
	bool pointer(QPointF position, bool press, bool release, bool constrain)
	{
		if (!program_)
			return false;
		if (!studioTarget_) {
			const auto *scaling = main_->findChild<QComboBox *>("previewScalingMode");
			if (!scaling || scaling->currentIndex() != 0 || !obs_frontend_preview_enabled()) {
				disarm();
				setStatus("Enable Draw again to fit and show the normal preview.");
				return true;
			}
		}
		const auto point =
			mapToCanvas(position, program_->size(), program_->devicePixelRatioF(), canvas_->size());
		if (press) {
			PaintStyle style;
			style.tool = tool_;
			style.color = color_;
			style.width = width_;
			style.opacity = opacity_;
			style.asset = asset_;
			style.imageWidth = imageWidth_;
			style.curvature = curvature_;
			style.markerSize = markerSize_;
			style.contrastOutline = contrastOutline_;
			strokeActive_ = point && canvas_->begin(*point, style);
			if (point && !strokeActive_)
				setStatus(
					"No drawing started. Check whether the layer is locked or hidden, and the image/stamp selection.");
			if (!point)
				return true; // Consume clicks in the letterbox, but create no ink.
		} else if (strokeActive_) {
			if (point)
				canvas_->extend(*point, constrain);
			else
				canvas_->breakSegment();
		}
		if (release) {
			canvas_->finish();
			strokeActive_ = false;
		}
		updateGuide();
		return true;
	}
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched == toolbarHost_) {
			if (event->type() == QEvent::Resize)
				adaptToolbar();
			return false;
		}
		if (watched != program_)
			return false;
		if (event->type() == QEvent::Resize)
			updateGuide();
		if (!armed_ || tool_ == Tool::None)
			return false;
		switch (event->type()) {
		case QEvent::Wheel:
		case QEvent::ContextMenu:
			// Scene editing/zoom gestures must not leak through live drawing.
			if (!studioTarget_) {
				event->accept();
				return true;
			}
			break;
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:
		case QEvent::MouseButtonRelease:
		case QEvent::MouseMove: {
			auto *mouse = static_cast<QMouseEvent *>(event);
			if (mouse->source() != Qt::MouseEventNotSynthesized) {
				event->accept();
				return true;
			}
			if (event->type() == QEvent::MouseMove) {
				if (!strokeActive_)
					return false;
				if (!(mouse->buttons() & Qt::LeftButton)) {
					canvas_->finish();
					strokeActive_ = false;
					return false;
				}
			} else if (mouse->button() != Qt::LeftButton)
				return !studioTarget_;
			event->accept();
			return pointer(mouse->position(),
				       event->type() == QEvent::MouseButtonPress ||
					       event->type() == QEvent::MouseButtonDblClick,
				       event->type() == QEvent::MouseButtonRelease,
				       mouse->modifiers() & Qt::ShiftModifier);
		}
		case QEvent::TabletPress:
		case QEvent::TabletMove:
		case QEvent::TabletRelease: {
			auto *tablet = static_cast<QTabletEvent *>(event);
			event->accept();
			return pointer(tablet->position(), event->type() == QEvent::TabletPress,
				       event->type() == QEvent::TabletRelease, tablet->modifiers() & Qt::ShiftModifier);
		}
		case QEvent::KeyPress: {
			auto *key = static_cast<QKeyEvent *>(event);
			if (key->key() == Qt::Key_Escape) {
				canvas_->cancel();
				disarm();
				return true;
			}
			if (key->matches(QKeySequence::Undo)) {
				finishGesture();
				canvas_->undo();
				syncLayers();
				return true;
			}
			if (key->matches(QKeySequence::Redo)) {
				finishGesture();
				canvas_->redo();
				syncLayers();
				return true;
			}
			if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
				finishGesture();
				canvas_->deleteSelection();
				updateGuide();
				return true;
			}
			if (!studioTarget_) {
				event->accept();
				return true;
			}
			break;
		}
		case QEvent::KeyRelease:
			if (!studioTarget_) {
				event->accept();
				return true;
			}
			break;
		case QEvent::Leave:
			if (strokeActive_ && isCursorTool(tool_))
				canvas_->breakSegment();
			break;
		case QEvent::FocusOut:
		case QEvent::UngrabMouse:
			canvas_->finish();
			strokeActive_ = false;
			break;
		default:
			break;
		}
		return false;
	}
	QPointer<QMainWindow> main_;
	std::shared_ptr<Canvas> canvas_;
	QPointer<QWidget> program_, toolbar_, secondaryToolbar_, toolbarHost_;
	bool arrangingToolbar_ = false;
	QPointer<QToolButton> drawButton_, colorButton_, appearanceButton_, layersButton_, undoButton_, redoButton_,
		clearButton_, moreButton_, shapeButton_;
	QPointer<QAction> shapeControlAction_;
	QPointer<ToolbarPopover> appearancePanel_, layersPanel_, toolsPanel_;
	QPointer<QWidget> widthRow_, assetGroup_;
	QPointer<QLabel> assetHint_, appearanceHint_;
	QPointer<QWidget> curveRow_, markerGroup_;
	QPointer<QSpinBox> markerNumber_, widthSpin_, opacitySpin_;
	QPointer<QCheckBox> outlineCheck_;
	QPointer<QListWidget> layerList_;
	QPointer<QSlider> layerSlider_;
	QPointer<SelectionGuide> guide_;
	QPointer<QToolButton> toolPickerButton_;
	std::array<QPointer<QAction>, 19> toolActions_;
	QPointer<QPushButton> assetButton_, addLayer_, removeLayer_, renameLayer_;
	bool layerMovePending_ = false, layerSyncPending_ = false;
	QPointer<QSpinBox> layerOpacity_, stampWidth_;
	QString statusText_;
	QTimer timer_;
	obs_source_t *source_ = nullptr;
	int channel_ = -1;
	bool armed_ = false, strokeActive_ = false, stopped_ = false, suspended_ = true;
	bool studioTarget_ = true;
	bool clearOnSceneChange_ = true;
	QColor color_{"#ff334f"};
	struct ToolPreset {
		QColor color{"#ff334f"};
		double width = 8, opacity = 1;
	};
	std::array<ToolPreset, 19> presets_;
	double curvature_ = 35, markerSize_ = 64;
	bool contrastOutline_ = true;
	double width_ = 8;
	double opacity_ = 1, imageWidth_ = 160;
	Tool tool_ = Tool::Pencil;
	Tool shapeTool_ = Tool::RectangleOutline;
	QImage asset_;
	Qt::FocusPolicy oldFocusPolicy_ = Qt::NoFocus;
};
static std::shared_ptr<Canvas> canvas;
static QPointer<Controller> controller;
} // namespace program_draw

bool obs_module_load(void)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main || !qApp)
		return false;
	program_draw::canvas = std::make_shared<program_draw::Canvas>();
	program_draw::registerInkSource(program_draw::canvas);
	program_draw::controller = new program_draw::Controller(main, program_draw::canvas);
	blog(LOG_INFO, "[program-draw] POC loaded; inline drawing controls support normal and Studio Mode");
	return true;
}
void obs_module_unload(void)
{
	delete program_draw::controller.data();
	program_draw::controller = nullptr;
	program_draw::canvas.reset();
}
