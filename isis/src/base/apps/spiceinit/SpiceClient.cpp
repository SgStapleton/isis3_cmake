#include "SpiceClient.h"

#include <sstream>

#include <QUrl>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QDomElement>

#include "Application.h"
#include "Constants.h"
#include "iException.h"
#include "iString.h"
#include "Pvl.h"
#include "Table.h"

using namespace std;
using namespace Isis;

namespace Isis {
  /**
   * This initializes a SpiceClient. It forms the XML to send and
   * starts in its own thread.
   *
   * @param url
   * @param port
   * @param cubeLabel
   * @param ckSmithed
   * @param ckRecon
   * @param ckNadir
   * @param ckPredicted
   * @param spkSmithed
   * @param spkRecon
   * @param spkPredicted
   * @param shape
   * @param startPad
   * @param endPad
   */
  SpiceClient::SpiceClient(iString url, int port, Pvl &cubeLabel,
      bool ckSmithed, bool ckRecon, bool ckNadir, bool ckPredicted,
      bool spkSmithed, bool spkRecon, bool spkPredicted,
      iString shape, double startPad, double endPad) : QThread() {
    p_xml = NULL;
    p_networkMgr = NULL;
    p_request = NULL;
    p_response = NULL;
    p_rawResponse = NULL;
    p_error = NULL;

    iString raw;
    p_xml = new iString();

    raw = "<input_label>\n";
    raw += "  <isis_version>\n";
    iString version = Application::Version();
    QByteArray isisVersionRaw(version.c_str());
    raw += iString(isisVersionRaw.toHex().constData()) + "\n";
    raw += "  </isis_version>\n";

    raw += "  <parameters>\n";
    raw += "    <cksmithed value='" + yesNo(ckSmithed) + "' />\n";
    raw += "    <ckrecon value='" + yesNo(ckRecon) + "' />\n";
    raw += "    <ckpredicted value='" + yesNo(ckPredicted) + "' />\n";
    raw += "    <cknadir value='" + yesNo(ckNadir) + "' />\n";
    raw += "    <spksmithed value='" + yesNo(spkSmithed) + "' />\n";
    raw += "    <spkrecon value='" + yesNo(spkRecon) + "' />\n";
    raw += "    <spkpredicted value='" + yesNo(spkPredicted) + "' />\n";
    raw += "    <shape value='" + shape + "' />\n";
    raw += "    <startpad time='" + iString(startPad) + "' />\n";
    raw += "    <endpad time='" + iString(endPad) + "' />\n";
    raw += "  </parameters>\n";

    raw += "  <label>\n";
    stringstream str;
    str << cubeLabel;
    raw += iString(QByteArray(str.str().c_str()).toHex().constData()) + "\n";

    raw += "  </label>\n";
    raw += "</input_label>";

    *p_xml = "name=" + iString(QByteArray(raw.c_str()).toHex().constData());

    int contentLength = p_xml->length();
    iString contentLengthStr = iString((BigInt)contentLength);

    p_request = new QNetworkRequest();
    p_request->setUrl(QUrl(url));
    p_request->setRawHeader("User-Agent", "SpiceInit 1.0");
    p_request->setHeader(QNetworkRequest::ContentTypeHeader,
                         "application/x-www-form-urlencoded");
    //p_request->setRawHeader("Content-Length", contentLengthStr.c_str());
    //p_request->setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute,
    //    true);

    moveToThread(this);
    start();
  }


  /**
   * This cleans up the spice client.
   *
   */
  SpiceClient::~SpiceClient() {
    if(p_xml) {
      delete p_xml;
      p_xml = NULL;
    }

    if(p_error) {
      delete p_error;
      p_error = NULL;
    }

    if(p_networkMgr) {
      delete p_networkMgr;
      p_networkMgr = NULL;
    }

    if(p_request) {
      delete p_request;
      p_request = NULL;
    }

    if(p_response) {
      delete p_response;
      p_response = NULL;
    }

    if(p_rawResponse) {
      delete p_rawResponse;
      p_rawResponse = NULL;
    }
  }


  /**
   * This POSTS to the spice server
   */
  void SpiceClient::sendRequest() {
    p_networkMgr = new QNetworkAccessManager();

    connect(p_networkMgr, SIGNAL(finished(QNetworkReply *)),
            this, SLOT(replyFinished(QNetworkReply *)));
    connect(p_networkMgr, SIGNAL(authenticationRequired(QNetworkReply *, QAuthenticator *)),
            this, SLOT(authenticationRequired(QNetworkReply *, QAuthenticator *)));
    connect(p_networkMgr, SIGNAL(proxyAuthenticationRequired(const QNetworkProxy &, QAuthenticator *)),
            this, SLOT(proxyAuthenticationRequired(const QNetworkProxy &, QAuthenticator *)));
    connect(p_networkMgr, SIGNAL(sslErrors(QNetworkReply *, const QList<QSslError> &)),
            this, SLOT(sslErrors(QNetworkReply *, const QList<QSslError> &)));

    p_networkMgr->post(*p_request, p_xml->c_str());
    exec();
  }


  /**
   * This is called when the server responds.
   *
   * @param reply
   */
  void SpiceClient::replyFinished(QNetworkReply *reply) {
    p_rawResponse = new iString(QString(reply->readAll()).toStdString());

    // Decode the response
    p_response = new iString();

    try {
      *p_response = iString(
          QByteArray::fromHex(QByteArray(p_rawResponse->c_str())).constData());

      // Make sure we can get the log out of it before continuing
      applicationLog();
    }
    catch(iException &e) {
      e.Clear();

      p_error = new iString();

      // Well, the XML is bad, maybe it's PVL
      try {
        Pvl pvlTest;
        stringstream s;
        s << *p_rawResponse;
        s >> pvlTest;

        PvlGroup &err = pvlTest.FindGroup("Error", Pvl::Traverse);

        *p_error = "The Spice Server was unable to initialize the cube.";

        if (err.FindKeyword("Message")[0] != "") {
          *p_error += "  The error reported was: ";
          *p_error += err.FindKeyword("Message")[0];
        }
      }
      catch(iException &e) {
        e.Clear();

        // Well, we really don't know what this is.
        *p_error = "The server sent an unrecognized response";

        if (*p_rawResponse != "") {
          *p_error += " [";
          *p_error += *p_rawResponse;
          *p_error += "]";
        }
      }
    }

    quit();
  }


  /**
   * Called if the server requires a authentication
   */
  void SpiceClient::authenticationRequired(QNetworkReply *, QAuthenticator *) {
    if(!p_response) {
      p_rawResponse = new iString();
      p_response = new iString();
    }

    *p_error = "Server expects authentication which is not currently ";
    *p_error += "supported";
    quit();
  }


  /**
   * Called if the server requires a authentication
   */
  void SpiceClient::proxyAuthenticationRequired(const QNetworkProxy &, QAuthenticator *) {
    if(!p_response) {
      p_rawResponse = new iString();
      p_response = new iString();
    }

    *p_error = "Server expects authentication which is not currently ";
    *p_error += "supported";
    quit();
  }


  /**
   * @param reply
   * @param err
   */
  void SpiceClient::sslErrors(QNetworkReply *reply,
                              const QList<QSslError> & err) {
    if(!p_response) {
      p_rawResponse = new iString();
      p_response = new iString();
    }

    *p_error = "Server expects authentication which is not currently ";
    *p_error += "supported";

    quit();
  }


  /**
   * This returns the root of the returned XML, throws an
   * appropriate error if the XML is wrong or missing.
   *
   * @return QDomElement
   */
  QDomElement SpiceClient::rootXMLElement() {
    if(!p_response || !p_rawResponse) {
      iString error = "No server response available";
      throw iException::Message(iException::Io, error, _FILEINFO_);
    }

    QDomDocument document;
    QString errorMsg;
    int errorLine, errorCol;

    if(!p_response->empty() &&
        document.setContent(QString(p_response->c_str()),
                            &errorMsg, &errorLine, &errorCol)) {
      return document.firstChild().toElement();
    }
    else {
      iString msg = "Unexpected response from spice server [";
      msg += *p_rawResponse;
      msg += "]";
      throw iException::Message(iException::Io, msg, _FILEINFO_);
    }
  }


  /**
   * This finds a tag (i.e. \<some_tag>) inside the current
   * element.
   *
   * @param currentElement
   * @param name
   *
   * @return QDomElement
   */
  QDomElement SpiceClient::findTag(QDomElement currentElement, iString name) {
    QString qtName = name.ToQt();
    for(QDomNode node = currentElement.firstChild();
        !node .isNull();
        node = node.nextSibling()) {
      QDomElement element = node.toElement();

      if(element.tagName() == qtName) {
        return element;
      }
    }

    iString msg = "Server response missing XML Tag [" + name + "]";
    throw iException::Message(iException::Io, msg, _FILEINFO_);
  }


  /**
   * This returns the spiceinit'd PvlGroup from the server.
   *
   * @return PvlGroup
   */
  PvlGroup SpiceClient::kernelsGroup() {
    checkErrors();

    QDomElement root = rootXMLElement();
    QDomElement kernelsLabel = findTag(root, "kernels_label");
    QString kernelsLabels = elementContents(kernelsLabel);

    iString unencoded(QByteArray::fromHex(kernelsLabels.toAscii()).constData());

    stringstream pvlStream;
    pvlStream << unencoded;

    Pvl labels;
    pvlStream >> labels;

    return labels.FindGroup("Kernels", Pvl::Traverse);
  }


  /**
   * This returns the PvlGroup we should log to the console
   *
   * @return PvlGroup
   */
  PvlGroup SpiceClient::applicationLog() {
    checkErrors();

    QDomElement root = rootXMLElement();
    QDomElement logLabel = findTag(root, "application_log");
    QString logLabels = elementContents(logLabel);

    iString unencoded(QByteArray::fromHex(logLabels.toAscii()).constData());

    stringstream pvlStream;
    pvlStream << unencoded;

    Pvl labels;
    pvlStream >> labels;

    return labels.FindGroup("Kernels", Pvl::Traverse);
  }


  /**
   * This returns the contents of the current element as a string.
   *   \<element>
   *     Contents
   *   \</element>
   *
   * @param element
   *
   * @return QString
   */
  QString SpiceClient::elementContents(QDomElement element) {
    return element.firstChild().toText().data();
  }


  /**
   * This returns the table given by the server. The ownership of the table is
   *   given to the caller.
   *
   * @return Table*
   */
  Table *SpiceClient::pointingTable() {
    return readTable("instrument_pointing", "InstrumentPointing");
  }


  /**
   * This returns the table given by the server. The ownership of the table is
   *   given to the caller.
   *
   * @return Table*
   */
  Table *SpiceClient::positionTable() {
    return readTable("instrument_position", "InstrumentPosition");
  }


  /**
   * This returns the table given by the server. The ownership of the table is
   *   given to the caller.
   *
   * @return Table*
   */
  Table *SpiceClient::bodyRotationTable() {
    return readTable("body_rotation", "BodyRotation");
  }


  /**
   * This returns the table given by the server.
   */
  Table *SpiceClient::sunPositionTable() {
    return readTable("sun_position", "SunPosition");
  }


  /**
   * This throws an exception if an error occurred
   */
  void SpiceClient::checkErrors() {
    if(p_error) {
      throw iException::Message(iException::Spice, *p_error, _FILEINFO_);
    }
  }


  /**
   * This yields the current thread until the server response is
   * received and initial (basic) processing is complete.
   */
  void SpiceClient::blockUntilComplete() {
    while(isRunning()) {
      yieldCurrentThread();
    }
  }


  PvlObject SpiceClient::naifKeywordsObject() {
    checkErrors();

    QDomElement root = rootXMLElement();
    QDomElement kernelsLabel = findTag(root, "kernels_label");
    QString kernelsLabels = elementContents(kernelsLabel);

    iString unencoded(QByteArray::fromHex(kernelsLabels.toAscii()).constData());

    stringstream pvlStream;
    pvlStream << unencoded;

    Pvl labels;
    pvlStream >> labels;

    return labels.FindObject("NaifKeywords");
  }


  /**
   * Converts a boolean to "yes" or "no"
   *
   * @param boolVal
   *
   * @return iString
   */
  iString SpiceClient::yesNo(bool boolVal) {
    if(boolVal)
      return "yes";
    else
      return "no";
  }


  Table *SpiceClient::readTable(iString xmlName, iString tableName) {
    checkErrors();

    QDomElement root = rootXMLElement();
    QDomElement tablesTag = findTag(root, "tables");
    QDomElement pointingTag = findTag(tablesTag, xmlName);
    QString encodedString = elementContents(pointingTag);

    QByteArray encodedArray;
    for (int i = 0; i < encodedString.size(); i++) {
      encodedArray.append(encodedString.data()[i]);
    }

    QByteArray unencodedArray(QByteArray::fromHex(encodedArray));

    stringstream tableStream;
    tableStream.write(unencodedArray.data(), unencodedArray.size());

    Pvl lab;
    tableStream >> lab;

    Table *table = new Table(tableName);
    table->Read(lab, tableStream);

    return table;

  }
};