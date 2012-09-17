#include "SnapshotModel.hpp"
#include "ProjectSettings.hpp"

#include "QOpenCV.hpp"
using namespace QOpenCV;

#include <qt-json/json.h>
using namespace QtJson;

#include <QFileInfo>
#include <QDir>
#include <QMap>
#include <QVector>
#include <QImage>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QGraphicsPolygonItem>
#include <QEvent>
#include <QGraphicsSceneMouseEvent>

bool SnapshotModel::s_staticInitialized = false;
QSet< QString > SnapshotModel::s_cacheableImages;
QSet< QString > SnapshotModel::s_resizedImages;

SnapshotModel::SnapshotModel(const QString& path, QObject *parent) :
    QObject(parent),
    m_originalPath(path),
    m_scene(new QGraphicsScene(this)),
    m_mode(INERT),
    m_color("green"),
    m_flann(0)
{
    if (!s_staticInitialized) {
        s_cacheableImages << "input";
        s_resizedImages << "input";
        s_staticInitialized = true;
    }

    m_pens["white"] = QPen(Qt::white);
    m_pens["thick-red"] = QPen(Qt::red, 2);

    m_scene->setObjectName("scene"); // so we can autoconnect signals

    qDebug() << "Loading" << qPrintable(path);

    // check if cache is present, create otherwise
    QFileInfo fi(path);

    QDir parent_dir = fi.absoluteDir();
    m_cacheDir = parent_dir.filePath( fi.baseName() + ".cache" );
    if (!m_cacheDir.exists()) {
        qDebug() << "creating cache directory" << qPrintable(m_cacheDir.path());
        parent_dir.mkdir( m_cacheDir.dirName() );
    }

    // add the image to the scene
    m_scene->addPixmap( QPixmap::fromImage( getImage("input") ) );
    m_scene->installEventFilter(this);

    loadData();

    qDebug() << qPrintable(path) << "loaded";
}

SnapshotModel::~SnapshotModel()
{
    qDebug() << "closing snapshot...";
    saveData();
}

bool SnapshotModel::eventFilter(QObject * target, QEvent * event)
{
    if (m_scene == qobject_cast<QGraphicsScene*>(target)) {
        switch( event->type() ) {
        case QEvent::GraphicsSceneMousePress: {
            QGraphicsSceneMouseEvent * mevent = dynamic_cast<QGraphicsSceneMouseEvent *>(event);
            // mevent->scenePos() is pixel coordinates of the pick
            if (mevent->button() == Qt::LeftButton) {
                pick( mevent->scenePos().x(), mevent->scenePos().y() );
            } else {
                unpick(mevent->scenePos().x(), mevent->scenePos().y());
            }
            break;
        }
        default:
            break;
        }
    }

    return true;
}

void SnapshotModel::pick(int x, int y)
{
    QImage input = getImage("input");

    if (m_mode == TRAIN) {
        if (input.rect().contains(x,y)) {
            addCross(x,y);
            selectByFlood(x,y);
            m_colorPicks[m_color].append( QPoint(x,y) ); // this is for saving / restoring
        }
    }
}

void SnapshotModel::setTrainMode(const QString &tag)
{
    if (tag.toLower() == "mask") {
        setMode(MASK);
    } else {
        setMode(TRAIN);
        m_color = tag;
    }

    updateViews();
}

void SnapshotModel::addCross(int x, int y)
{
    QGraphicsLineItem * cross = new QGraphicsLineItem(-3,-3,+3,+3, layer(m_color));
    cross->setZValue(5);
    cross->setPen(m_pens["thick-red"]);
    cross->setPos(x, y);

    QGraphicsLineItem * l = new QGraphicsLineItem(+3,-3,-3,+3,cross);
    l->setPen(m_pens["thick-red"]);

    l = new QGraphicsLineItem(+2,-2,-2,+2,cross);
    l->setPen(m_pens["white"]);
    l = new QGraphicsLineItem(+2,+2,-2,-2,cross);
    l->setPen(m_pens["white"]);

    updateViews();
}

void SnapshotModel::updateViews()
{
    QString tag = m_mode == TRAIN ? m_color : QString();
    foreach(QString layer_name, m_layers.keys()) {
        m_layers[layer_name]->setVisible( (layer_name == tag) );
    }
    /* something wrong with repaintage... */
    foreach(QGraphicsView * view,m_scene->views()) {
        view->viewport()->update();
    }
}

void SnapshotModel::saveData()
{
    QFile file( m_cacheDir.filePath("data.json"));
    file.open(QFile::WriteOnly);
    QTextStream strm(&file);
    QVariantHash conf;
    QVariantHash picks;
    foreach(QString color, m_colorPicks.keys()) {
        QVariantList l;
        foreach(QPoint p, m_colorPicks[color])
            l << p.x() << p.y();
        picks[color] = l;
    }
    conf["picks"] = picks;
    strm << Json::serialize(conf);
}

void SnapshotModel::loadData()
{
    QFile file( m_cacheDir.filePath("data.json"));
    file.open(QFile::ReadOnly);
    QTextStream strm(&file);
    QString json = strm.readAll();
    QVariantMap parsed = Json::parse(json).toMap();

    { // load the picks
        Mode old_mode = m_mode;
        QString old_color = m_color;

        QVariantMap picks = parsed["picks"].toMap();
        foreach(QString color, picks.keys()) {
            setTrainMode(color);
            QVariantList list = picks[color].toList();
            QVector<QVariant> array;
            foreach(QVariant v, list) array << v;

            for(int i=0; i<array.size(); i+=2) {
                int x = array[i].toInt(), y = array[i+1].toInt();
                pick(x,y);
            }
        }

        m_mode = old_mode;
        m_color = old_color;
    }

    updateViews();
}

QGraphicsItem * SnapshotModel::layer(const QString &name)
{
    if (!m_layers.contains(name)) {
        m_layers[name] = new QGraphicsItemGroup(0,m_scene);
    }
    return m_layers[name];
}

void SnapshotModel::clearLayer()
{
    if (m_mode == TRAIN) {
        QList<QGraphicsItem*> children = layer( m_color )->childItems();
        foreach(QGraphicsItem* child, children) {
            delete child;
        }
        m_colorPicks[m_color].clear();
    } else if (m_mode == MASK) {
        qDebug() << "TODO clear mask";
    }

    updateViews();
}

void SnapshotModel::selectByFlood(int x, int y)
{
    // flood fill inside roi
    cv::Mat input = getMatrix("lab");
    cv::Mat mask = getMatrix(m_color+"_pickMask");
    cv::Rect bounds;

    int pf = projectSettings().value("pick_fuzz").toInt();
    int res = cv::floodFill(input, mask,
                            cv::Point(x,y),
                            cv::Scalar(0,0,0),
                            &bounds,
                            // FIXME the range should be preference
                            cv::Scalar( pf,pf,pf ),
                            cv::Scalar( pf,pf,pf ),
                            cv::FLOODFILL_MASK_ONLY | cv::FLOODFILL_FIXED_RANGE );

    if (res < 1) return;

    // if intersected some polygons - remove these polygons and grow ROI with their bounds
    QRect q_bounds = toQt(bounds);
    QGraphicsItem * colorLayer = layer(m_color);
    foreach(QGraphicsItem * item, colorLayer->childItems()) {
        QGraphicsPolygonItem * poly_item = qgraphicsitem_cast<QGraphicsPolygonItem *>(item);
        if (!poly_item) continue;
        QRect bounds = poly_item->polygon().boundingRect().toRect();
        if (q_bounds.intersects( bounds )) {
            q_bounds = q_bounds.united(bounds);
            delete poly_item;
        }
    }
    // intersect with the image
    q_bounds = q_bounds.intersected( getImage("input").rect() );

    bounds = toCv(q_bounds);

    // cut out from mask
    cv::Mat mask0;
    cv::Mat( mask, cv::Rect( bounds.x+1,bounds.y+1,bounds.width,bounds.height ) ).copyTo(mask0);
    // find contours there and add to the polygons list
    std::vector< std::vector< cv::Point > > contours;
    cv::findContours(mask0,
                     contours,
                     CV_RETR_EXTERNAL,
                     CV_CHAIN_APPROX_TC89_L1,
                     // offset: compensate for ROI and flood fill mask border
                     bounds.tl() );

    // now refresh contour visuals
    foreach( const std::vector< cv::Point >& contour, contours ) {
        std::vector< cv::Point > approx;
        // simplify contours
        cv::approxPolyDP( contour, approx, 3, true);
        QPolygon polygon = toQPolygon(approx);
        QGraphicsPolygonItem * poly_item = new QGraphicsPolygonItem( polygon, colorLayer );
        poly_item->setPen(m_pens["thick-red"]);
        poly_item->setZValue(5);

        poly_item = new QGraphicsPolygonItem( polygon, poly_item );
        poly_item->setPen(m_pens["white"]);
    }

}

void SnapshotModel::unpick(int x, int y)
{
    // 1. find which contour we're in (shouldn't we capture it elsewhere then?)
    QString color;
    QGraphicsPolygonItem * unpicked_poly = 0;

    foreach(QString c, m_layers.keys()) {
        QGraphicsItem * l = layer(c);
        foreach(QGraphicsItem * item, l->childItems()) {
            QGraphicsPolygonItem * poly_item = qgraphicsitem_cast<QGraphicsPolygonItem *>(item);
            if (!poly_item) continue;
            if (poly_item->polygon().containsPoint(QPointF(x,y), Qt::OddEvenFill)) {
                unpicked_poly = poly_item;
                color = c;
                break;
            }
        }
        if (unpicked_poly)
            break;
    }
    if (!unpicked_poly)
        return;

    // 2. find which picks are in this contour and remove them and their crosses
    foreach(const QPoint& pick, m_colorPicks[color]) {
        if (unpicked_poly->polygon().containsPoint(pick, Qt::OddEvenFill))
            m_colorPicks[color].removeOne(pick);
    }
    foreach(QGraphicsItem * item, layer(color)->childItems()) {
        QGraphicsLineItem * cross = qgraphicsitem_cast<QGraphicsLineItem *>(item);
        if (!cross) continue;
        if (unpicked_poly->polygon().containsPoint( cross->pos(), Qt::OddEvenFill )) {
            delete cross;
        }
    }

    // 3. draw this contour onto the mask
    cv::Mat mask = getMatrix(color+"_pickMask");
    std::vector< cv::Point > contour = toCvInt( unpicked_poly->polygon() );
    cv::Point * pts[] = { contour.data() };
    int npts[] = { contour.size() };
    cv::fillPoly( mask, (const cv::Point**)pts, npts, 1, cv::Scalar(0), 8, 0,
                  // compensate for mask border
                  cv::Point(1,1) );

    // 4. delete the polygon itself
    delete unpicked_poly;

    updateViews();
}

void SnapshotModel::trainColors()
{
    if (m_displayers.contains("samples-display")) {
        delete m_displayers["samples-display"];
    }
    QGraphicsItemGroup * displayer = new QGraphicsItemGroup(0,m_scene);
    m_displayers["samples-display"] = displayer;

    QVector<cv::Mat> centers_list;
    int centers_count = 0;
    cv::Mat input = getMatrix("lab");

    int color_index = 0;

    foreach(QString color, m_layers.keys()) {
        if (!m_matrices.contains(color+"_pickMask")) continue;

        QVector<ColorType> sample_pixels;
        cv::Mat mask = getMatrix(color+"_pickMask");

        for(int i = 0; i<input.rows; ++i)
            for(int j = 0; j<input.cols; ++j)
                if (mask.at<unsigned char>(i+1,j+1))
                    // copy this pixel
                    sample_pixels
                            << input.ptr<ColorType>(i)[j*3]
                            << input.ptr<ColorType>(i)[j*3+1]
                            << input.ptr<ColorType>(i)[j*3+2];

        if (sample_pixels.size()==0) continue;

        qDebug() << "collected" << sample_pixels.size() / 3 << qPrintable(color) << "pixels";

        cv::Mat sample(sample_pixels.size()/3, 3, CV_32FC1, sample_pixels.data());
        // cv::flann::hierarchicalClustering returns float centers even for integer features
        cv::Mat centers(COLOR_GRADATIONS, 3, CV_32FC1);
        cvflann::KMeansIndexParams params(
                    COLOR_GRADATIONS, // branching
                    10, // max iterations
                    cvflann::FLANN_CENTERS_KMEANSPP,
                    0);
        int n_clusters = cv::flann::hierarchicalClustering< ColorDistance >( sample, centers, params );
        centers_list << centers;
        centers_count += n_clusters;
        color_index++;
    }

    cv::Mat featuresLab = cv::Mat( centers_count, 3, CV_32FC1 );
    for(int i=0; i<centers_list.size(); ++i)
        centers_list[i].copyTo( featuresLab.rowRange( i*COLOR_GRADATIONS,(i+1)*COLOR_GRADATIONS ) );
    setMatrix("featuresLab", featuresLab);

    cv::Mat featuresRGB = getMatrix("featuresRGB");
    QImage palette( featuresRGB.data, centers_count, 1, QImage::Format_RGB888 );
    QGraphicsPixmapItem * gpi = new QGraphicsPixmapItem( QPixmap::fromImage(palette), displayer );
    gpi->scale(15,15);

    cvflann::AutotunedIndexParams params( 0.8, 1, 0, 1.0 );
    //cvflann::LinearIndexParams params;
    if (m_flann) delete m_flann;
    m_flann = new cv::flann::GenericIndex< ColorDistance > (featuresLab, params);
    qDebug() << "built FLANN classifier";

    updateViews();
}

void SnapshotModel::countCards()
{
    if (!m_flann) {
        qDebug() << "Teach me the colors first";
        return;
    }
    cv::Mat input = getMatrix("lab");

    int n_pixels = input.rows * input.cols;
    cv::Mat indices( input.rows, input.cols, CV_32SC1 );
    cv::Mat dists( input.rows, input.cols, CV_32FC1 );

    cv::Mat input_1 = input.reshape( 1, n_pixels ),
            indices_1 = indices.reshape( 1, n_pixels ),
            dists_1 = dists.reshape( 1, n_pixels );

    qDebug() << "K-Nearest Neighbout Search";
    cvflann::SearchParams params(cvflann::FLANN_CHECKS_UNLIMITED, 0);
    m_flann->knnSearch( input_1, indices_1, dists_1, 1, params);
    qDebug() << "K-Nearest Neighbout Search done";

    m_matrices["indices"] = indices;
    m_matrices["dists"] = dists;

    cv::Mat cards_mask;
    float thresh = 1000.0;
    cv::threshold(dists, cards_mask, thresh, 0, cv::THRESH_TRUNC);
    cards_mask.convertTo( cards_mask, CV_8UC1, - 255.0 / thresh, 255.0 );

    // display results
    if (m_displayers.contains("knn-display")) {
        delete m_displayers["knn-display"];
    }

    // poor man's LookUpTable
    cv::Mat vision = cv::Mat( indices.rows, indices.cols, CV_8UC3, cv::Scalar(0,0,0,0) );
    cv::Mat lut = getMatrix("featuresRGB");
    for(int i=0; i<n_pixels; i++) {
        if (cards_mask.data[i]) {
            int index = indices.ptr<int>(0)[i];
            vision.data[i*3] = lut.data[ index*3 ];
            vision.data[i*3+1] = lut.data[ index*3 + 1 ];
            vision.data[i*3+2] = lut.data[ index*3 + 2 ];
        }
    }

    QGraphicsItemGroup * displayer = new QGraphicsItemGroup(0,m_scene);
    m_displayers["knn-display"] = displayer;
    QImage vision_image( (unsigned char *)vision.data, vision.cols, vision.rows, QImage::Format_RGB888 );
    QGraphicsPixmapItem * gpi = new QGraphicsPixmapItem( QPixmap::fromImage(vision_image), displayer );

    updateViews();
}

QImage SnapshotModel::getImage(const QString &tag)
{
    if (!m_images.contains(tag)) {
        QImage img;
        int size_limit = projectSettings().value("size_limit", 640).toInt();

        if (s_cacheableImages.contains(tag)) {
            // if cacheable - see if we can load it
            QString path = m_cacheDir.filePath(tag + ".png");
            if (img.load(path)) {
                if (tag.endsWith("mask",Qt::CaseInsensitive))
                    img = img.convertToFormat(QImage::Format_Indexed8, greyTable());
                else
                    img = img.convertToFormat(QImage::Format_RGB888);
            }

            // see if size is compatible
            if (s_resizedImages.contains(tag) && !img.isNull() && std::max(img.width(), img.height()) != size_limit)
                img = QImage();
        }
        if (img.isNull()) {
            if (tag == "input") {
                img = QImage( m_originalPath )
                        .scaled( size_limit, size_limit, Qt::KeepAspectRatio, Qt::SmoothTransformation )
                        .convertToFormat(QImage::Format_RGB888);
            }
        }

        setImage(tag,img);
    }

    return m_images[tag];
}

cv::Mat SnapshotModel::getMatrix(const QString &tag)
{
    if (!m_matrices.contains(tag)) {
        cv::Mat matrix;
        // create some well known matrices
        QSize qsz = getImage("input").size();
        cv::Size inputSize = cv::Size( qsz.width(), qsz.height() );

        if (tag == "lab") {
            cv::Mat input = getMatrix("input");
            input.convertTo(matrix, CV_32FC3, 1.0/255.0);
            cv::cvtColor( matrix, matrix, CV_RGB2Lab );
        } else if (tag.endsWith("_pickMask")) {
            matrix = cv::Mat(inputSize.height+2, inputSize.width+2, CV_8UC1, cv::Scalar(0));
        } else if (tag == "featuresRGB") {
            cv::Mat featuresLab = getMatrix("featuresLab");
            matrix = cv::Mat( featuresLab.rows, 3, CV_32FC1 );
            cv::cvtColor( cv::Mat(featuresLab.rows, 1, CV_32FC3, featuresLab.data),
                          cv::Mat(featuresLab.rows, 1, CV_32FC3, matrix.data),
                          CV_Lab2RGB );
            matrix.convertTo( matrix, CV_8UC1, 255.0 );
        } else if (tag == "input") {
            QImage img = getImage(tag);
            matrix = cv::Mat( img.height(), img.width(), CV_8UC3, (void*)img.constBits() );
        } else if (tag == "cacheable_mask") { // <-- FIXME just an example
            QImage img = getImage(tag);
            matrix = cv::Mat( img.height(), img.width(), CV_8UC1, (void*)img.constBits() );
        }

        setMatrix(tag, matrix);
    }
    return m_matrices[tag];
}

void SnapshotModel::setMatrix(const QString &tag, const cv::Mat &matrix)
{
    m_matrices[tag] = matrix;
}

void SnapshotModel::setImage(const QString &tag, const QImage &img)
{
    m_images[tag] = img;
}

