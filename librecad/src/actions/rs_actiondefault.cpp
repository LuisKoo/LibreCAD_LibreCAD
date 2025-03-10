/****************************************************************************
**
** This file is part of the LibreCAD project, a 2D CAD program
**
** Copyright (C) 2010 R. van Twisk (librecad@rvt.dds.nl)
** Copyright (C) 2001-2003 RibbonSoft. All rights reserved.
**
**
** This file may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file gpl-2.0.txt included in the
** packaging of this file.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
** This copyright notice MUST APPEAR in all copies of the script!
**
**********************************************************************/
#include <algorithm>

#include<QMouseEvent>

#include "qc_applicationwindow.h"
#include "rs_actiondefault.h"
#include "rs_commandevent.h"
#include "rs_debug.h"
#include "rs_dialogfactory.h"
#include "rs_graphicview.h"
#include "rs_line.h"
#include "rs_modification.h"
#include "rs_overlaybox.h"
#include "rs_preview.h"
#include "rs_selection.h"
#include "rs_settings.h"
#include "rs_units.h"

struct RS_ActionDefault::Points {
    RS_Vector v1;
    RS_Vector v2;

    // Number of temporary entities for glowing effects
    unsigned nHighLightDuplicates = 0;

    RS_Entity* highlightedEntity = nullptr;
};

namespace {

// Glowing effects on Mouse hover
constexpr double minimumHoverTolerance =  3.0;

constexpr double hoverToleranceFactor1 =  1.0;
constexpr double hoverToleranceFactor2 = 10.0;

// whether the entity supports glowing effects on mouse hovering
bool allowMouseOverGlowing(const RS_Entity* entity)
{
    if (entity == nullptr)
        return false;
    switch (entity->rtti())
    {
    case RS2::EntityHatch:
    case RS2::EntityImage:
    case RS2::EntitySolid:
    case RS2::EntityUnknown:
    case RS2::EntityPattern:
    return false;
    default:
    return true;
    }
}
}

/**
 * Constructor.
 */
RS_ActionDefault::RS_ActionDefault(RS_EntityContainer& container,
                                   RS_GraphicView& graphicView)
    : RS_PreviewActionInterface("Default",
								container, graphicView)
	, pPoints(std::make_unique<Points>())
    , snapRestriction(RS2::RestrictNothing)
{

    RS_DEBUG->print("RS_ActionDefault::RS_ActionDefault");
    setActionType(RS2::ActionDefault);
    typeToSelect = graphicView.getTypeToSelect();
    RS_DEBUG->print("RS_ActionDefault::RS_ActionDefault: OK");
}

RS_ActionDefault::~RS_ActionDefault() = default;


void RS_ActionDefault::init(int status) {
    RS_DEBUG->print("RS_ActionDefault::init");
    if (status >= 0){
        checkSupportOfQuickEntityInfo();
    }
    if(status==Neutral){
        deletePreview();
        deleteSnapper();
    }
    RS_PreviewActionInterface::init(status);
    pPoints->v1 = pPoints->v2 = {};
    //    snapMode.clear();
    //    snapMode.restriction = RS2::RestrictNothing;
    //    restrBak = RS2::RestrictNothing;
    //        RS_DIALOGFACTORY->requestToolBar(RS2::ToolBarMain);

    RS_DEBUG->print("RS_ActionDefault::init: OK");
}

void RS_ActionDefault::checkSupportOfQuickEntityInfo(){
    LC_QuickInfoWidget *entityInfoWidget = QC_ApplicationWindow::getAppWindow()->getEntityInfoWidget();
    if (entityInfoWidget != nullptr){
        this->allowEntityQuickInfoAuto = entityInfoWidget->isAutoSelectEntitiesInDefaultAction();
        this->allowEntityQuickInfoForCTRL = entityInfoWidget->isSelectEntitiesInDefaultActionWithCTRL();
    }
}

void RS_ActionDefault::keyPressEvent(QKeyEvent* e) {
    //        std::cout<<"RS_ActionDefault::keyPressEvent(): begin"<<std::endl;
    switch(e->key()){
    case Qt::Key_Shift:
        snapRestriction = snapMode.restriction;
        setSnapRestriction(RS2::RestrictOrthogonal);
        e->accept();
        break; //avoid clearing command line at shift key
        //cleanup default action, issue#285
    case Qt::Key_Escape:
        //        std::cout<<"RS_ActionDefault::keyPressEvent(): Qt::Key_Escape"<<std::endl;
        deletePreview();
        deleteSnapper();
        setStatus(Neutral);
        e->accept();
        break;
    default:
        e->ignore();
    }

}

void RS_ActionDefault::keyReleaseEvent(QKeyEvent* e) {
    if (e->key()==Qt::Key_Shift) {
        setSnapRestriction(snapRestriction);
        e->accept();
    }
}


/*
   Highlights hovered entities that are visible and not locked.

   - by Melwyn Francis Carlo <carlo.melwyn@outlook.com>
*/
void RS_ActionDefault::highlightHoveredEntities(QMouseEvent* event)
{
    clearHighLighting();

    bool shouldShowQuickInfoWidget = allowEntityQuickInfoAuto || (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier) && allowEntityQuickInfoForCTRL);

    auto guard = RS_SETTINGS->beginGroupGuard("/Appearance");
    bool showHighlightEntity = RS_SETTINGS->readNumEntry("/VisualizeHovering", 0) != 0 || shouldShowQuickInfoWidget;
    if (!showHighlightEntity)
        return;

    RS_Entity* entity = catchEntity(event);
    if (entity == nullptr)
        return;
    if (!entity->isVisible() || (entity->isLocked() && !shouldShowQuickInfoWidget)){
        return;
    }

    const double hoverToleranceFactor = (entity->rtti() == RS2::EntityEllipse)
                                        ? hoverToleranceFactor1
                                        : hoverToleranceFactor2;

    const double hoverTolerance { hoverToleranceFactor / graphicView->getFactor().magnitude() };

    double hoverTolerance_adjusted = ((entity->rtti() != RS2::EntityEllipse) && (hoverTolerance < minimumHoverTolerance))
                                     ? minimumHoverTolerance
                                     : hoverTolerance;

    double screenTolerance = graphicView->toGraphDX( 0.01*std::min(graphicView->getWidth(), graphicView->getHeight()));
    hoverTolerance_adjusted = std::min(hoverTolerance_adjusted, screenTolerance);
    bool isPointOnEntity = false;

    RS_Vector currentMousePosition = graphicView->toGraph(event->position());
    if (((entity->rtti() >= RS2::EntityDimAligned) && (entity->rtti() <= RS2::EntityDimLeader))
            ||   (entity->rtti() == RS2::EntityText)       || (entity->rtti() == RS2::EntityMText))
    {
        double nearestDistanceTo_pointOnEntity = 0.;

        entity->getNearestPointOnEntity(currentMousePosition, true, &nearestDistanceTo_pointOnEntity);

        if (nearestDistanceTo_pointOnEntity <= hoverTolerance_adjusted) isPointOnEntity = true;
    }
    else
    {
        isPointOnEntity = entity->isPointOnEntity(currentMousePosition, hoverTolerance_adjusted);
    }

    // Glowing effect on mouse hovering
    if (isPointOnEntity){
        highlightEntity(entity);
        if (shouldShowQuickInfoWidget){
            updateQuickInfoWidget(entity);
        }
    }
}

void RS_ActionDefault::mouseMoveEvent(QMouseEvent* e) {

    RS_Vector mouse = graphicView->toGraph(e->position());
    RS_Vector relMouse = mouse - graphicView->getRelativeZero();

    RS_DIALOGFACTORY->updateCoordinateWidget(mouse, relMouse);

    // clear any existing hovering
    clearHighLighting();

    switch (getStatus()) {
    case Neutral:
        deleteSnapper();
        highlightHoveredEntities(e);
        break;
    case Dragging:
        //v2 = graphicView->toGraph(e->position());
		pPoints->v2 = mouse;

		if (graphicView->toGuiDX(pPoints->v1.distanceTo(pPoints->v2))>10) {
            // look for reference points to drag:
            double dist;
			RS_Vector ref = container->getNearestSelectedRef(pPoints->v1, &dist);
            if (ref.valid==true && graphicView->toGuiDX(dist)<8) {
                RS_DEBUG->print("RS_ActionDefault::mouseMoveEvent: "
                                "moving reference point");
                setStatus(MovingRef);
				pPoints->v1 = ref;
				graphicView->moveRelativeZero(pPoints->v1);
            }
            else {
                // test for an entity to drag:
				RS_Entity* en = catchEntity(pPoints->v1);
				if (en && en->isSelected()) {
                    RS_DEBUG->print("RS_ActionDefault::mouseMoveEvent: "
                                    "moving entity");
                    setStatus(Moving);
					RS_Vector vp= en->getNearestRef(pPoints->v1);
					if(vp.valid) pPoints->v1=vp;

                    //graphicView->moveRelativeZero(v1);
                }

                // no entity found. start area selection:
                else {
                    setStatus(SetCorner2);
                }
            }
        }
        break;

    case MovingRef:
		pPoints->v2 = snapPoint(e);
		RS_DIALOGFACTORY->updateCoordinateWidget(pPoints->v2, pPoints->v2 - graphicView->getRelativeZero());

        if (e->modifiers() & Qt::ShiftModifier) {
            mouse = snapToAngle(mouse, pPoints->v1);
            pPoints->v2 = mouse;
        }

        deletePreview();
        preview->addSelectionFrom(*container);
		preview->moveRef(pPoints->v1, pPoints->v2 - pPoints->v1);

        if (e->modifiers() & Qt::ShiftModifier) {
            RS_Line *line = new RS_Line(pPoints->v1, mouse);
            preview->addEntity(line);
            line->setSelected(true);
        }

        drawPreview();
        break;

    case Moving:
		pPoints->v2 = snapPoint(e);
		RS_DIALOGFACTORY->updateCoordinateWidget(pPoints->v2, pPoints->v2 - graphicView->getRelativeZero());

        if (e->modifiers() & Qt::ShiftModifier) {
            mouse = snapToAngle(mouse, pPoints->v1);
            pPoints->v2 = mouse;
        }

        deletePreview();
        preview->addSelectionFrom(*container);
		preview->move(pPoints->v2 - pPoints->v1);

        if (e->modifiers() & Qt::ShiftModifier) {
            RS_Line *line = new RS_Line(pPoints->v1, mouse);
            preview->addEntity(line);
            line->setSelected(true);
        }

        drawPreview();
        break;

    case SetCorner2:
		if (pPoints->v1.valid) {
			pPoints->v2 = mouse;

            deletePreview();

			RS_OverlayBox* ob=new RS_OverlayBox(preview.get(),
												RS_OverlayBoxData(pPoints->v1, pPoints->v2));
            preview->addEntity(ob);

            drawPreview();
        }
        break;
    case Panning:
    {
        RS_Vector const vTarget{e->position()};
		RS_Vector const v01=vTarget - pPoints->v1;
        if(v01.squared()>=64.){
            graphicView->zoomPan((int) v01.x, (int) v01.y);
			pPoints->v1=vTarget;
        }
    }
        break;

    default:
        break;
    }
}



void RS_ActionDefault::mousePressEvent(QMouseEvent* e) {
    if (e->button()==Qt::LeftButton) {
        switch (getStatus()) {
        case Neutral:
        {
            auto const m=e->modifiers();
            if(m & (Qt::ControlModifier|Qt::MetaModifier)){
                pPoints->v1 = RS_Vector{e->position()};
                setStatus(Panning);
            } else {
                pPoints->v1 = graphicView->toGraph(e->position());
                setStatus(Dragging);
            }
        }
            break;

        case Moving: {
			pPoints->v2 = snapPoint(e);
            if (e->modifiers() & Qt::ShiftModifier) {
                pPoints->v2 = snapToAngle(pPoints->v2, pPoints->v1);
            }
            deletePreview();
            RS_Modification m(*container, graphicView);
            RS_MoveData data;
            data.number = 0;
            data.useCurrentLayer = false;
            data.useCurrentAttributes = false;
			data.offset = pPoints->v2 - pPoints->v1;
            m.move(data);
            setStatus(Neutral);
            RS_DIALOGFACTORY->updateSelectionWidget(
                        container->countSelected(),container->totalSelectedLength());
            deleteSnapper();
        }
            break;

        case MovingRef: {
			pPoints->v2 = snapPoint(e);
            if (e->modifiers() & Qt::ShiftModifier) {
                pPoints->v2 = snapToAngle(pPoints->v2, pPoints->v1);
            }
            deletePreview();
            RS_Modification m(*container, graphicView);
            RS_MoveRefData data;
			data.ref = pPoints->v1;
			data.offset = pPoints->v2 - pPoints->v1;
            m.moveRef(data);
            //container->moveSelectedRef(v1, v2-v2);
            setStatus(Neutral);
            RS_DIALOGFACTORY->updateSelectionWidget(
                        container->countSelected(),container->totalSelectedLength());
        }
            break;

        default:
            break;
        }
    } else if (e->button()==Qt::RightButton) {
        //cleanup
        setStatus(Neutral);
        e->accept();
    }
}



void RS_ActionDefault::mouseReleaseEvent(QMouseEvent* e) {
    RS_DEBUG->print("RS_ActionDefault::mouseReleaseEvent()");

    if (e->button()==Qt::LeftButton) {
        pPoints->v2 = graphicView->toGraph(e->position());
        switch (getStatus()) {
        case Dragging: {
            // select single entity:
            RS_Entity* en = catchEntity(e);

            if (en != nullptr) {
                deletePreview();

                RS_Selection s(*container, graphicView);

                s.selectSingle(en);

                RS_DIALOGFACTORY->updateSelectionWidget(
                            container->countSelected(),container->totalSelectedLength());

                e->accept();

                setStatus(Neutral);
            } else {
                setStatus(SetCorner2);
            }
        }
            break;

        case SetCorner2: {
            //v2 = snapPoint(e);
            pPoints->v2 = graphicView->toGraph(e->position());

            // select window:
            //if (graphicView->toGuiDX(v1.distanceTo(v2))>20) {
            deletePreview();

			bool cross = (pPoints->v1.x > pPoints->v2.x);
            RS_Selection s(*container, graphicView);
            bool select = (e->modifiers() & Qt::ShiftModifier) == 0;
			s.selectWindow(typeToSelect, pPoints->v1, pPoints->v2, select, cross);

            RS_DIALOGFACTORY->updateSelectionWidget(
                        container->countSelected(),container->totalSelectedLength());

            setStatus(Neutral);
            e->accept();
            //}
        }
            break;

        case Panning:
            setStatus(Neutral);
            break;

        default:
            break;

        }
    } else if (e->button()==Qt::RightButton) {
        //cleanup
        setStatus(Neutral);
        e->accept();
    }
}

void RS_ActionDefault::commandEvent(RS_CommandEvent* e) {
    QString c = e->getCommand().toLower();

    // if the current action can't deal with the command,
    //   it might be intended to launch a new command
    //if (!e.isAccepted()) {
    // command for new action:
    //RS2::ActionType type = RS_COMMANDS->cmdToAction(c);
    //if (type!=RS2::ActionNone) {
    //graphicView->setCurrentAction(type);
    //return true;
    //}
    //}
}



QStringList RS_ActionDefault::getAvailableCommands() {
    QStringList cmd;

    //cmd += "line";
    //cmd += "rectangle";

    return cmd;
}


void RS_ActionDefault::updateMouseButtonHints() {
    switch (getStatus()) {
    case Neutral:
		RS_DIALOGFACTORY->updateMouseWidget();
        break;
    case SetCorner2:
        RS_DIALOGFACTORY->updateMouseWidget(tr("Choose second edge"),
                                            tr("Back"));
        break;
    default:
		RS_DIALOGFACTORY->updateMouseWidget();
        break;
    }
}

void RS_ActionDefault::updateMouseCursor() {
    switch (getStatus()) {
    case Neutral:
        graphicView->setMouseCursor(RS2::ArrowCursor);
        break;
    case Moving:
    case MovingRef:
        graphicView->setMouseCursor(RS2::SelectCursor);
        break;
    case Panning:
        graphicView->setMouseCursor(RS2::ClosedHandCursor);
        break;
    default:
        break;
    }
}

void RS_ActionDefault::clearHighLighting()
{
    auto* hContainer = graphicView->getOverlayContainer(RS2::OverlayEffects);
    if (hContainer->count()==0)
        return;
    hContainer->clear();
    pPoints->highlightedEntity=nullptr;
    graphicView->redraw(RS2::RedrawOverlay);
    clearQuickInfoWidget();
}

void RS_ActionDefault::resume()
{
    clearHighLighting();
    checkSupportOfQuickEntityInfo();
    BASE_CLASS::resume();
}

void RS_ActionDefault::suspend()
{
    clearHighLighting();
    BASE_CLASS::suspend();
}

void RS_ActionDefault::highlightEntity(RS_Entity* entity) {
    if (!allowMouseOverGlowing(entity))
        return;

    // The container for highlighting effects
    auto hContainer = graphicView->getOverlayContainer(RS2::OverlayEffects);
    hContainer->clear();

    pPoints->highlightedEntity = entity;

    RS_Entity* duplicatedEntity = pPoints->highlightedEntity->clone();

    duplicatedEntity->reparent(hContainer);
    duplicatedEntity->setHighlighted(true);
    hContainer->addEntity(duplicatedEntity);

    graphicView->redraw(RS2::RedrawOverlay);
}

RS2::EntityType RS_ActionDefault::getTypeToSelect(){
    return typeToSelect;
}

void RS_ActionDefault::clearQuickInfoWidget(){
    LC_QuickInfoWidget *entityInfoWidget = QC_ApplicationWindow::getAppWindow()->getEntityInfoWidget();
    if (entityInfoWidget != nullptr){
//        entityInfoWidget->processEntity(nullptr);
    }
}

void RS_ActionDefault::updateQuickInfoWidget(RS_Entity *pEntity){
    LC_QuickInfoWidget *entityInfoWidget = QC_ApplicationWindow::getAppWindow()->getEntityInfoWidget();
    if (entityInfoWidget != nullptr){
        entityInfoWidget->processEntity(pEntity);
    }
}
// EOF
