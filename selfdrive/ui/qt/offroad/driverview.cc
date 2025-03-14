#include "selfdrive/ui/qt/offroad/driverview.h"

#include <QPainter>

#include "selfdrive/ui/qt/qt_window.h"
#include "selfdrive/ui/qt/util.h"

const int FACE_IMG_SIZE = 130;

DriverViewWindow::DriverViewWindow(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_OpaquePaintEvent);
  layout = new QStackedLayout(this);
  layout->setStackingMode(QStackedLayout::StackAll);

  cameraView = new CameraWidget("camerad", VISION_STREAM_DRIVER, true, this);
  layout->addWidget(cameraView);

  scene = new DriverViewScene(this);
  connect(cameraView, &CameraWidget::vipcThreadFrameReceived, scene, &DriverViewScene::frameUpdated);
  layout->addWidget(scene);
  layout->setCurrentWidget(scene);
}

void DriverViewWindow::mouseReleaseEvent(QMouseEvent* e) {
  cameraView->stopVipcThread();
  emit done();
}

#ifndef QCOM
DriverViewScene::DriverViewScene(QWidget* parent) : sm({"driverStateV2"}), QWidget(parent) {
  face_img = loadPixmap("../assets/img_driver_face_static.png", {FACE_IMG_SIZE, FACE_IMG_SIZE});
#else
DriverViewScene::DriverViewScene(QWidget* parent) : sm({"driverState"}), QWidget(parent) {
  face_img = loadPixmap("../assets/img_driver_face_qcom.png", {FACE_IMG_SIZE, FACE_IMG_SIZE});
#endif
}

void DriverViewScene::showEvent(QShowEvent* event) {
  frame_updated = false;
  #ifdef QCOM
  is_rhd = params.getBool("IsRhdDetected");
  #endif
  params.putBool("IsDriverViewEnabled", true);
}

void DriverViewScene::hideEvent(QHideEvent* event) {
  params.putBool("IsDriverViewEnabled", false);
}

void DriverViewScene::frameUpdated() {
  frame_updated = true;
  sm.update(0);
  update();
}

#ifndef QCOM
void DriverViewScene::paintEvent(QPaintEvent* event) {
  QPainter p(this);

  // startup msg
  if (!frame_updated) {
    p.setPen(Qt::white);
    p.setRenderHint(QPainter::TextAntialiasing);
    configFont(p, "Inter", 100, "Bold");
    p.drawText(geometry(), Qt::AlignCenter, tr("camera starting"));
    return;
  }

  cereal::DriverStateV2::Reader driver_state = sm["driverStateV2"].getDriverStateV2();
  cereal::DriverStateV2::DriverData::Reader driver_data;

  is_rhd = driver_state.getWheelOnRightProb() > 0.5;
  driver_data = is_rhd ? driver_state.getRightDriverData() : driver_state.getLeftDriverData();

  bool face_detected = driver_data.getFaceProb() > 0.7;
  if (face_detected) {
    auto fxy_list = driver_data.getFacePosition();
    auto std_list = driver_data.getFaceOrientationStd();
    float face_x = fxy_list[0];
    float face_y = fxy_list[1];
    float face_std = std::max(std_list[0], std_list[1]);

    float alpha = 0.7;
    if (face_std > 0.15) {
      alpha = std::max(0.7 - (face_std-0.15)*3.5, 0.0);
    }
    const int box_size = 220;
    // use approx instead of distort_points
    int fbox_x = 1080.0 - 1714.0 * face_x;
    int fbox_y = -135.0 + (504.0 + std::abs(face_x)*112.0) + (1205.0 - std::abs(face_x)*724.0) * face_y;
    p.setPen(QPen(QColor(255, 255, 255, alpha * 255), 10));
    p.drawRoundedRect(fbox_x - box_size / 2, fbox_y - box_size / 2, box_size, box_size, 35.0, 35.0);
  }

  // icon
  const int img_offset = 60;
  const int img_x = is_rhd ? rect().right() - FACE_IMG_SIZE - img_offset : rect().left() + img_offset;
  const int img_y = rect().bottom() - FACE_IMG_SIZE - img_offset;
  p.setOpacity(face_detected ? 1.0 : 0.2);
  p.drawPixmap(img_x, img_y, face_img);
}
#else
void DriverViewScene::paintEvent(QPaintEvent* event) {
  QPainter p(this);

  // startup msg
  if (!frame_updated) {
    p.setPen(Qt::white);
    p.setRenderHint(QPainter::TextAntialiasing);
    configFont(p, "Inter", 100, "Bold");
    p.drawText(geometry(), Qt::AlignCenter, tr("camera starting"));
    return;
  }

  const int width = 4 * height() / 3;
  const QRect rect2 = {rect().center().x() - width / 2, rect().top(), width, rect().height()};
  const QRect valid_rect = {is_rhd ? rect2.right() - rect2.height() / 2 : rect2.left(), rect2.top(), rect2.height() / 2, rect2.height()};

  // blackout
  const QColor bg(0, 0, 0, 140);
  const QRect& blackout_rect = rect2;
  p.fillRect(blackout_rect.adjusted(0, 0, valid_rect.left() - blackout_rect.right(), 0), bg);
  p.fillRect(blackout_rect.adjusted(valid_rect.right() - blackout_rect.left(), 0, 0, 0), bg);

  // face bounding box
  cereal::DriverState::Reader driver_state = sm["driverState"].getDriverState();
  bool face_detected = driver_state.getFaceProb() > 0.5;
  if (face_detected) {
    auto fxy_list = driver_state.getFacePosition();
    auto std_list = driver_state.getFaceOrientationStd();
    float face_x = fxy_list[0];
    float face_y = fxy_list[1];
    float face_std = std::max(std_list[0], std_list[1]);

    float alpha = 0.7;
    if (face_std > 0.08) {
      alpha = std::max(0.7 - (face_std-0.08)*7, 0.0);
    }
    const int box_size = 0.6 * rect2.height() / 2;
    const float rhd_offset = 0.05; // lhd is shifted, so rhd is not mirrored
    int fbox_x = valid_rect.center().x() + (is_rhd ? (face_x + rhd_offset) : -face_x) * valid_rect.width();
    int fbox_y = valid_rect.center().y() + face_y * valid_rect.height();
    p.setPen(QPen(QColor(255, 255, 255, alpha * 255), 10));
    p.drawRoundedRect(fbox_x - box_size / 2, fbox_y - box_size / 2, box_size, box_size, 35.0, 35.0);
  }

  // icon
  const int img_offset = 30;
  const int img_x = is_rhd ? rect2.right() - FACE_IMG_SIZE - img_offset : rect2.left() + img_offset;
  const int img_y = rect2.bottom() - FACE_IMG_SIZE - img_offset;
  p.setOpacity(face_detected ? 1.0 : 0.3);
  p.drawPixmap(img_x, img_y, face_img);
}
#endif
