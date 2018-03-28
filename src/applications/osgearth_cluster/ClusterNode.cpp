#include "ClusterNode"

ClusterNode::ClusterNode(MapNode* mapNode) :
    _radius(50),
    _mapNode(mapNode),
    _nextLabel(0),
    _enabled(true)
{
    setNumChildrenRequiringUpdateTraversal(1);
    setCullingActive(false);

    _defaultImage = osgDB::readRefImageFile("../data/placemark32.png");
    _horizon = new Horizon();
}

void ClusterNode::addNode(PlaceNode* node)
{
    _placeNodes.push_back(node);
}

void ClusterNode::removeNode(PlaceNode* node)
{
    PlaceNodeList::iterator itr = std::find(_placeNodes.begin(), _placeNodes.end(), node);
    if (itr != _placeNodes.end())
    {
        _placeNodes.erase(itr);
    }
}

unsigned int ClusterNode::getRadius() const
{
    return _radius;
}

void ClusterNode::setRadius(unsigned int radius)
{
    _radius = radius;
}

bool ClusterNode::getEnabled() const
{
    return _enabled;
}

void ClusterNode::setEnabled(bool enabled)
{
    _enabled = enabled;
}

StyleClusterCallback* ClusterNode::getStyleCallback()
{
    return _styleCallback.get();
}

void
ClusterNode::setStyleCallback(StyleClusterCallback* callback)
{
    _styleCallback = callback;
}

void ClusterNode::getClusters(osg::Camera* camera, ClusterList& out)
{
    _nextLabel = 0;

    osg::Viewport* viewport = camera->getViewport();
    if (!viewport)
    {
        return;
    }

    osg::Matrixd mvpw = camera->getViewMatrix() *
        camera->getProjectionMatrix() *
        camera->getViewport()->computeWindowMatrix();

    std::vector<TPoint> points;

    std::vector< osg::ref_ptr< PlaceNode > > validPlaces;

    for (unsigned int i = 0; i < _placeNodes.size(); i++)
    {
        osg::Vec3d world;
        _placeNodes[i]->getPosition().toWorld(world);

        if (!_horizon->isVisible(world))
        {
            continue;
        }

        osg::Vec3d screen = world * mvpw;

        if (screen.x() >= 0 && screen.x() <= viewport->width() &&
            screen.y() >= 0 && screen.y() <= viewport->height())
        {
            validPlaces.push_back(_placeNodes[i]);
            points.push_back({ screen.x(), screen.y() });
        }
    }

    if (validPlaces.size() == 0) return;

    kdbush::KDBush<TPoint> index(points);
    std::set< unsigned int > clustered;

    for (unsigned int i = 0; i < validPlaces.size(); i++)
    {
        TPoint &screen = points[i];
        PlaceNode* place = validPlaces[i].get();

        // If this thing is already part of a cluster then just continue.
        if (clustered.find(i) != clustered.end())
        {
            continue;
        }

        // Get any matching indices that are part of this cluster.
        TIds indices;
        index.within(screen.first, screen.second, _radius, [&indices](const auto id) { indices.push_back(id); });

        // Create a new cluster.
        Cluster cluster;

        unsigned int actualCount = 0;

        // Add all of the points to the cluster.
        for (unsigned int j = 0; j < indices.size(); j++)
        {
            if (clustered.find(indices[j]) == clustered.end())
            {
                cluster.places.push_back(validPlaces[indices[j]]);
                actualCount++;
                clustered.insert(indices[j]);
            }
        }

        std::stringstream buf;
        buf << actualCount << std::endl;

        PlaceNode* marker = getOrCreateLabel();
        marker->setPosition(place->getPosition());
        marker->setText(buf.str());

        cluster.marker = marker;
        out.push_back(cluster);

        clustered.insert(i);
    }
}

void ClusterNode::traverse(osg::NodeVisitor& nv)
{
    if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR && _enabled)
    {
        osgUtil::CullVisitor* cv = nv.asCullVisitor();

        osg::Vec3d eye, center, up;
        cv->getCurrentCamera()->getViewMatrixAsLookAt(eye, center, up);

        _horizon->setEye(eye);

        ClusterList clusters;
        getClusters(cv->getCurrentCamera(), clusters);

        for (ClusterList::iterator itr = clusters.begin(); itr != clusters.end(); ++itr)
        {
            Cluster& cluster = *itr;
            // If we have more than 1 place, traverse the representative marker
            if (cluster.places.size() > 1)
            {
                if (_styleCallback)
                {
                    (*_styleCallback)(cluster);
                }

                itr->marker->accept(nv);
            }
            else
            {
                // Otherwise just traverse the first node
                cluster.places[0]->accept(nv);
            }


        }
    }
    else
    {
        for (PlaceNodeList::iterator itr = _placeNodes.begin(); itr != _placeNodes.end(); ++itr)
        {
            itr->get()->accept(nv);
        }
    }
}

PlaceNode* ClusterNode::getOrCreateLabel()
{
    PlaceNode* node = 0;
    if (_labelPool.size() <= _nextLabel)
    {
        // set up a style to use for placemarks:
        Style placeStyle;
        placeStyle.getOrCreate<TextSymbol>()->declutter() = false;
        node = new PlaceNode(_mapNode.get(), GeoPoint(SpatialReference::create("wgs84"), 0, 0, 0), _defaultImage.get(), "", placeStyle);
        node->setDynamic(true);
        _labelPool.push_back(node);
    }
    else
    {
        node = _labelPool[_nextLabel];
    }

    ++_nextLabel;

    return node;


}