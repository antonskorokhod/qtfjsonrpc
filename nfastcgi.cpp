/*
 * FastCGI processing module for QT5
 * @autor Anton Skorokhod <anton@nsl.cz>
 */

#include <QSocketNotifier>
#include <QDebug>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonArray>
#include <QThread>

#include "nfastcgi.h"

NFastCgi::NFastCgi(const char *socketPath, int serviceMetaType, int threadCount = 0, QObject *parent) : QObject(parent), m_notifier(0), m_serviceMetaType(serviceMetaType)
{
    // инициализация метаданных до создания потоков, чтобы потом не повредить данные
    NNamedService *serviceInit = static_cast<NNamedService *>(QMetaType::create(m_serviceMetaType));
    Q_CHECK_PTR(serviceInit);
    serviceInit->parseMetaInfo();
    QMetaType::destroy(m_serviceMetaType, (void *) serviceInit);

    // создание пула потоков
    jobsPool = new QThreadPool(this);
    jobsPool->setMaxThreadCount(threadCount == 0 ? QThread::idealThreadCount() : threadCount);

    // инициализаяиц fastcgi и подключения сигнала к сокету
    FCGX_Init();
    int sock = FCGX_OpenSocket(socketPath, 1024);
    this->m_notifier = new QSocketNotifier(sock, QSocketNotifier::Read);
    QObject::connect(this->m_notifier, SIGNAL(activated(int)), this, SLOT(connectionPending(int)));
}

void NFastCgi::connectionPending(int socket)
{
    QSocketNotifier* notifier = qobject_cast<QSocketNotifier*>(this->sender());
    Q_CHECK_PTR(notifier);

    notifier->setEnabled(false);

    FCGX_Request* req = new FCGX_Request;
    FCGX_InitRequest(req, socket, 0);
    int s = FCGX_Accept_r(req);
    if (s >= 0) {
        /* Здесь создаём и запускаем поток */
        NFastCgiJob *newJob = new NFastCgiJob(req, m_serviceMetaType);
        jobsPool->start(newJob);
    }

    notifier->setEnabled(true);
}

const char *NFastCgiJob::getJsonError(NNamedService::NSException *e)
{
  int code = e->getCode();
  int id = e->getId();
  QString message = e->getMessage();

  if (message.isEmpty()) {
      switch (code) {
        case NNamedService::NSException::code_parseError: message = "Parse error"; break;
        case NNamedService::NSException::code_invalidRequest: message = "Invalid Request"; break;
        case NNamedService::NSException::code_methodNotFound:message = "Method not found"; break;
        case NNamedService::NSException::code_invalidParams: message = "Invalid params"; break;
        case NNamedService::NSException::code_internalError: message = "Internal error"; break;
        case NNamedService::NSException::code_serverError: message = "Server error"; break;
        default: message = "unknown error";
      }
  }

  return QString("{\"jsonrpc\": \"2.0\", \"error\": {\"code\": %1, \"message\": \"%2\"}, \"id\": %3}")
                   .arg(code)
                   .arg(message)
                   .arg((id < 1) ? "null" : QString("\"%1\"").arg(id))
                   .toUtf8().data();

}

NFastCgiJob::NFastCgiJob(FCGX_Request *request, int serviceMetaType) : m_request(request), m_serviceMetaType(serviceMetaType), request_ip(0) { }

NFastCgiJob::~NFastCgiJob()
{
    FCGX_Finish_r(m_request);
}

void NFastCgiJob::run()
{
    FCGX_FPrintF(m_request->out, "Content-type: application/json; charset=UTF-8\r\n");
    FCGX_FPrintF(m_request->out, "Expires: Wed, 23 Mar 1983 12:15:00 GMT\r\n"\
                 "Cache-Control: no-store, no-cache, must-revalidate\r\n"\
                 "Cache-Control: post-check=0, pre-check=0\r\n"\
                 "Pragma: no-cache\r\n"\
                 "\r\n");

    char *post_data = 0;
    try {
        // для начала читаем весь массив пост-запроса, чтобы иметь возможность спарсить json
        char *contentLength = FCGX_GetParam("CONTENT_LENGTH", m_request->envp);
        int len = 0;
        if (contentLength != NULL) { len = strtol(contentLength, NULL, 10); }

        if ((len < 1) || (len > NFastCgiJob::MAX_REQUEST_SIZE))
            throw NNamedService::NSException(NNamedService::NSException::code_invalidRequest);

        post_data = new char[len+1];
        if (!post_data)
            throw NNamedService::NSException(NNamedService::NSException::code_serverError, QString("falied to alloc %1 bytes").arg(len));

        if(FCGX_GetStr(post_data, len, m_request->in) != len)
            throw NNamedService::NSException(NNamedService::NSException::code_serverError, QString("falied to read %1 bytes").arg(len));

        // у нас уже есть данные, теперь можно попробовать распарсить jsonrpc-запрос

        QByteArray result = processRequest(QByteArray(post_data, len), FCGX_GetParam("REMOTE_ADDR", m_request->envp));

        FCGX_PutS(result.data(), m_request->out);

    } catch (NNamedService::NSException &e) {
        FCGX_FPrintF(m_request->out, getJsonError(&e));
        qDebug() << "NFastCgiJob::run() -> " << getJsonError(&e);
    }
    if (post_data) delete post_data;
    FCGX_Finish_r(m_request);
}

QByteArray NFastCgiJob::processRequest(QByteArray request, QString ip)
{
    QJsonParseError json_error;
    json_request = QJsonDocument::fromJson(request, &json_error);
    if (json_error.error != QJsonParseError::NoError)
        throw NNamedService::NSException(NNamedService::NSException::code_parseError);

    // далее имея json-документ необходимо проверить десяток условий верности запроса
    if (!json_request.isObject())
        throw NNamedService::NSException(NNamedService::NSException::code_invalidRequest);
    QJsonObject json_root = json_request.object();

    // нам нужен только jsonrpc версии 2.0
    if ((!json_root.contains("jsonrpc")) || (json_root.value("jsonrpc").toString("") != "2.0"))
        throw NNamedService::NSException(NNamedService::NSException::code_invalidRequest);

    if ((!json_root.contains("method")) || (!json_root.value("method").isString()))
        throw NNamedService::NSException(NNamedService::NSException::code_invalidRequest);

    if ((!json_root.contains("params")) || (!json_root.value("params").isArray()))
        throw NNamedService::NSException(NNamedService::NSException::code_invalidRequest);

    if ((!json_root.contains("id")) || (!json_root.value("id").isString()))
        throw NNamedService::NSException(NNamedService::NSException::code_invalidRequest);

    // TODO: тут будет авторизация :)

    // создание экземпляра класса сервиса
    NNamedService *service = static_cast<NNamedService *>(QMetaType::create(m_serviceMetaType));

    // а тут заполнение дополнительных данных!!!
    if (!ip.isEmpty()) {
        service->params["remote_ip"] = ip;
    }

    // подготовка класса для ответа
    QVariantMap jresult;

    // собственно вызов функции
    jresult.insert("result", service->process(json_root.value("method").toString(), json_root.value("params").toArray().toVariantList()));

    // чистка памяти
    QMetaType::destroy(m_serviceMetaType, (void *) service);

    // дополнительные параметры ответа
    jresult.insert("jsonrpc", "2.0");
    jresult.insert("id", json_root.value("id").toVariant());

    QJsonDocument result(QJsonObject::fromVariantMap(jresult));

    return result.toJson();
}
