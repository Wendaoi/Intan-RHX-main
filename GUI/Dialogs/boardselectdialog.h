//------------------------------------------------------------------------------
//
//  Intan Technologies RHX Data Acquisition Software
//  Version 3.4.0
//
//  Copyright (c) 2020-2025 Intan Technologies
//
//  This file is part of the Intan Technologies RHX Data Acquisition Software.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published
//  by the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//  This software is provided 'as-is', without any express or implied warranty.
//  In no event will the authors be held liable for any damages arising from
//  the use of this software.
//
//  See <http://www.intantech.com> for documentation and product information.
//
//------------------------------------------------------------------------------

#ifndef BOARDSELECTDIALOG_H
#define BOARDSELECTDIALOG_H

#include <QDialog>
#include <QVector>
#include <QTableWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QSplashScreen>
#include <QStyle>
#include <QIcon>
#include <QString>

// Include the necessary headers for custom types
#include "rhxcontroller.h"
#include "rhxglobals.h"
#include "systemstate.h"
#include "controllerinterface.h"
#include "commandparser.h"
#include "controlwindow.h"
#include "startupdialog.h"
#include "demodialog.h"
#include "Engine/API/Synthetic/syntheticrhxcontroller.h"
#include "Engine/API/Synthetic/playbackrhxcontroller.h"

// Forward declarations for custom classes
class BoardIdentifier;
class DataFileReader;
class AbstractRHXController;

enum UsbVersion {
    USB2,
    USB3,
    USB3_7310
};

// Structure for controller information
struct ControllerInfo {
    QString serialNumber;
    UsbVersion usbVersion;
    BoardMode boardMode;
    int numSPIPorts;
    bool expConnected;
};

class BoardIdentifier : public QObject
{
public:
    BoardIdentifier(QWidget *parent_);
    ~BoardIdentifier();

    QVector<ControllerInfo*> getConnectedControllersInfo();
    static QString getBoardTypeString(BoardMode mode, int numSpiPorts);
    static QIcon getIcon(const QString& boardType, QStyle *style, int size);

private:
    void identifyController(ControllerInfo *controller, int index);
    QString opalKellyModelName(int model) const;
    bool uploadFpgaBitfileQMessageBox(const QString& filename);

    okCFrontPanel *dev;
    QVector<ControllerInfo*> controllers;
    QWidget *parent;
};

class BoardSelectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BoardSelectDialog(QWidget *parent = nullptr);
    ~BoardSelectDialog();

    static bool validControllersPresent(QVector<ControllerInfo*> cInfo);

private slots:
    void openSelectedBoard();
    void newRowSelected(int row);
    void startBoard(int row);
    void playbackDataFile();
    void advanced();

private:
    void populateTable();
    QSize calculateTableSize();
    void showDemoMessageBox();
    void startSoftware(ControllerType controllerType, AmplifierSampleRate sampleRate, StimStepSize stimStepSize,
                       int numSPIPorts, bool expanderConnected, const QString& boardSerialNumber, 
                       AcquisitionMode mode, bool is7310, DataFileReader* dataFileReader = nullptr);

    QTableWidget *boardTable;
    QPushButton *openButton;
    QPushButton *playbackButton;
    QPushButton *advancedButton;
    
    QCheckBox *defaultSampleRateCheckBox;
    QCheckBox *defaultSettingsFileCheckBox;
    
    QSplashScreen* splash;
    QString splashMessage;
    Qt::Alignment splashMessageAlign;
    QColor splashMessageColor;
    
    BoardIdentifier *boardIdentifier;
    QVector<ControllerInfo*> controllersInfo;
    DataFileReader *dataFileReader;
    
    AbstractRHXController *rhxController;
    SystemState *state;
    ControllerInterface *controllerInterface;
    CommandParser *parser;
    ControlWindow *controlWindow;
    
    bool useOpenCL;
    uint8_t playbackPorts;
};

#endif // BOARDSELECTDIALOG_H